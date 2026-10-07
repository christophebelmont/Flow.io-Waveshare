# MQTTModule (`moduleId: mqtt`)

## Rôle

Cœur MQTT unifié (transport + scheduling TX):
- connexion broker, souscriptions RX, reconnexion/backoff
- chemin TX unique pour **toutes** les publications MQTT
- files de jobs par priorité (`High`, `Normal`, `Low`)
- build topic/payload **à la demande** dans le buffer central
- dédup/coalescing par clé logique `(producerId, messageId)`
- exposition du service `MqttService`

Type: module actif.

## Dépendances

- `loghub`
- `wifi`
- `cmd`
- `time`
- `alarms`

## Affinité / cadence

- core: `0`
- task: `mqtt`
- stack: `Limits::Mqtt::TaskStackSize` (`5712`)
- loop delay: `Limits::Mqtt::Timing::LoopDelayMs` (`50 ms`)

## Services exposés

- `mqtt` -> `MqttService`
  - `enqueue(ctx, producerId, messageId, prio, flags)`
  - `registerProducer(ctx, producer)`
  - `formatTopic(ctx, suffix, out, outLen)`
  - `isConnected(ctx)`

Contrat producteur (`MqttPublishProducer`):
- `buildMessage(ctx, messageId, buildCtx)`
- `onMessagePublished(...)`
- `onMessageDeferred(...)`
- `onMessageDropped(...)`
- `onTransportTick(...)` (optionnel)

## Services consommés

- `wifi` (`WifiService`)
- `cmd` (`CommandService`)
- `config` (`ConfigStoreService`)
- `time.scheduler` (`TimeSchedulerService`)
- `alarms` (`AlarmService`)
- `eventbus` (`EventBusService`)
- `datastore` (`DataStoreService`)

## Config / NVS

Module config: `mqtt`
- `host`
- `port`
- `user`
- `pass`
- `baseTopic`
- `topicDeviceId` (optionnel, segment `<deviceId>` des topics)
- `deviceName` (optionnel, nom affiché pour Home Assistant Discovery `device.name`)
- `enabled`

Identité config interne:
- `moduleId = ConfigModuleId::Mqtt`
- `localBranchId = 1` (route cfg MQTT locale)

## RX MQTT

Topics souscrits:
- `<base>/<device>/cmd` (QoS 0)
- `<base>/<device>/cfg/set` (QoS 1)

Pipeline RX:
1. callback IDF `MQTT_EVENT_DATA`
2. copie vers queue RX bornée (`RxMsg`)
3. traitement dans la task `mqtt`
4. ACK/erreur sur `ack` ou `cfg/ack`

Queue RX:
- longueur: `Limits::Mqtt::Capacity::RxQueueLen` (`8`)
- création: `xQueueCreateStatic(...)`
- stockage: heap alloué une fois à l'init (`heap_caps_malloc`)

## TX unifié (jobs)

### Structure logique

Un job ne contient pas de topic/payload persistants:
- `producerId`
- `messageId`
- `priority`
- `flags`
- `retryCount`
- `notBeforeMs`

Capacités:
- slots jobs globaux: `MaxJobs = 80`
- ring `High`: `80`
- ring `Normal`: `80`
- ring `Low`: `60`

Politique:
- ordonnanceur priorité `High -> Normal -> Low`
- budget de traitement: `ProcessBudgetPerTick = 8`
- retry centralisé: backoff exponentiel `250 ms -> 10 s`
- coalescing: un seul job vivant par `(producerId,messageId)`

### Producteurs enregistrés (principaux)

- ACK (`producerId=1`)
- Status (`producerId=2`)
- Runtime (`producerId=4`, via `RuntimeProducer`)
- Alarm (`producerId=5`)
- Config routes MQTT internes (`producerId=41`)
- autres modules autoportés (HA, IO, PoolDevice, PoolLogic, Wifi, Time, etc.) via `MqttConfigRouteProducer`

## Topics principaux

- `<base>/<device>/status` (retain, LWT online/offline)
- `<base>/<device>/ack`
- `<base>/<device>/cfg/ack`
- `<base>/<device>/cfg/*` (publiés par producteurs config module-owned)
- `<base>/<device>/rt/*` (runtime producer + publishers périodiques)

Référence détaillée: `../core/mqtt-topics.md`.

Note:
- si `mqtt.topicDeviceId` est vide, l'ID topic est auto-généré depuis la MAC (`ESP32-XXXXXX`)
- si renseigné, il est utilisé pour `<base>/<deviceId>/...` et le `client_id` MQTT

## EventBus

Abonnements:
- `DataChanged`
- `ConfigChanged`
- `AlarmRaised`, `AlarmCleared`, `AlarmReset`, `AlarmSilenceChanged`, `AlarmConditionChanged`

Effets:
- `DataChanged(DATAKEY_WIFI_READY)`: gating connexion MQTT
- `ConfigChanged` sur clés de connexion MQTT: reconnect propre
- événements alarmes: enqueue des jobs `rt/alarms/*`

## DataStore

Écritures runtime:
- `MqttReady`
- compteurs RX: `rxDrop`, `oversizeDrop`, `parseFail`, `handlerFail`

## Signification des logs MQTT

### Occupation queues/jobs

`queue occ max/boot jobs=A/240 qh=B/80 qn=C/80 ql=D/192`
- niveau log: `DEBUG` (métrologie uniquement, pas d'alerte à elle seule)
- max observés depuis le boot (non remis à zéro)
- `jobs`: slots jobs utilisés
- `qh/qn/ql`: profondeur max des rings High/Normal/Low

### Rejet enqueue (noyau)

`enqueue reject reason=<slot_full|queue_full> producer=P msg=M prio=R jobs=...`
- `slot_full`: plus de slot job libre (`240/240`)
- `queue_full`: ring de priorité cible saturé
- peut être silencé par flag `MqttEnqueueFlags::SilentRejectLog`

### Publications acceptées en attente

`enqueue deferred reason=promotion_full producer=P msg=M prio=R ...`
- la publication reste acceptée : l'ancienne entrée reste valide si la promotion échoue
- la priorité souhaitée est conservée et la promotion est retentée automatiquement
- les diagnostics enqueue sont limités à un par seconde et respectent `SilentRejectLog`

`queue state jobs=A queued=B processing=C waiting=D`
- niveau `DEBUG`, occupation instantanée relevée sous le même verrou, toutes les 5 s
- `jobs = queued + processing + waiting`
- `waiting` : jobs conservés en attente d'une place ou de l'échéance de retry
- les compteurs physiques `qh/qn/ql` peuvent aussi inclure des références obsolètes

Les jobs acceptés ont un état explicite : libre, en file, en traitement ou en
attente de file. Les reprises et promotions sont parcourues par priorité puis
avec un curseur circulaire, sans remettre à zéro les délais de retry. Une nouvelle
demande sur le même couple producteur/message est fusionnée avec le job conservé.
Les jobs survivent à la déconnexion et reprennent après reconnexion. Une nouvelle
publication sans slot ou sans place initiale reste refusée (`enqueue=false`).

Validation native : `python3 -m unittest scripts.tests.test_mqtt_queue`.

### Métriques cfg route producer

`cfgq p=.. e=.. rf=.. rt=.. ok=.. to=.. tf=.. tt=.. tk=.. tto=..`
- `p`: routes cfg pending (global)
- `e`: routes en attente de ré-enqueue transport
- `rf`: refus enqueue fenêtre 5s
- `rt`: tentatives retry fenêtre 5s
- `ok`: retry acceptés fenêtre 5s
- `to`: timeouts fenêtre 5s (route perdue après `10 s` de refus)
- `tf/tt/tk/tto`: cumuls boot (`refus/tries/ok/timeouts`)
- niveau log: `DEBUG` par défaut, `WARN` si `tto > 0` (au moins un timeout cumulé depuis le boot)

### Logs IDF MQTT client

Exemple:
`MQTT_CLIENT: mqtt_message_receive: transport_read() error: errno=128`
- erreur transport bas niveau TCP/TLS côté client IDF
- déclenche en pratique une déconnexion puis le cycle de reconnexion/backoff du module

## Synchronisation différée des configurations

`MqttConfigRouteProducer` conserve les demandes de publication dans une boîte fixe de 96 routes, avec priorité maximale par route et protection des accès concurrents. Les callbacks de configuration et `requestFullSync` enregistrent uniquement les demandes. La tâche MQTT vérifie sa disponibilité, déclenche une synchronisation à la reconnexion, puis traite au plus deux nouvelles demandes par producteur et par passage. Les refus de la file MQTT utilisent le mécanisme existant de réessai. Le producteur n’a plus d’abonnement `DataChanged` pour déclencher la synchronisation complète depuis l’EventBus.

## Diagnostic de saturation cohérent

Le diagnostic TX prend un `QueueSnapshot` sous `jobsMux_`, le verrou qui protège
les transitions des jobs, les anneaux et les compteurs d'admission. Les logs sont
émis après déverrouillage. Les lignes `queue at=<millis>` d'un rapport proviennent
du même snapshot ; elles sont séparées pour respecter les 160 caractères de
LogHub. Le rapport périodique est émis toutes les cinq secondes au niveau INFO.

Invariants :

- `free + used = MaxJobs` (192 sur Waveshare).
- `used = queued + processing + waiting`.
- `queued = live(high) + live(normal) + live(low)` ; chaque job `Queued` a
  exactement une référence valide dans sa file effective.
- Pour chaque file : `physical = live + stale`.
- `admitted - released = used` (arithmétique des compteurs uint32).

`WaitingForQueue` est un état occupé hors file, utilisé pour le backoff et la
réadmission. `Processing` conserve le slot pendant la construction et l'appel
transport. Une promotion de priorité invalide l'ancienne référence par son jeton,
sans la retirer immédiatement de l'ancien anneau. Ainsi, **195 entrées physiques
pour 192 slots est possible sans fuite**, indépendamment de toute incohérence de
lecture. Les maxima historiques `max/boot` sont indépendants : leur somme n'est
pas une occupation instantanée.

Mesures :

- `oldest_ms` : âge maximal des slots occupés, sans remise à zéro lors d'une
  coalescence, promotion ou reprise après échec.
- `enqueue_mHz` : nouvelles admissions par seconde multipliées par 1000, mesurées
  entre deux rapports. Les coalescences sont comptées séparément.
- `dequeue_mHz` : passages en `Processing` par seconde multipliés par 1000,
  **reprises incluses** ; ce n'est pas le débit de livraison au broker.
- `mean_slot_ms` : résidence moyenne des slots libérés depuis le démarrage,
  y compris les abandons après erreur définitive. Les slots encore occupés ne
  participent pas à cette moyenne.
- `queue rejects` : rejets d'admission pour saturation, par producteur, type
  diagnostique et priorité ; `delta` depuis le rapport précédent, `total` depuis
  le boot. Inclut `SilentRejectLog` et les rejets masqués par la limitation des
  logs détaillés. Les demandes refusées avant admission (transport déconnecté,
  priorité invalide, stockage absent) ne font pas partie de ces compteurs.

La table statique `MqttDiagnostics::Sources` associe des identifiants et des
intervalles numériques à des noms. Aucune comparaison de topic ou recherche de
chaîne n'intervient. `type` est un identifiant diagnostique local au producteur ;
`msg` reste l'identifiant existant transmis au builder. Les producteurs futurs
doivent être ajoutés à la table ; à défaut leurs rejets apparaissent dans
`unregistered`, et leur identifiant brut reste présent dans les logs détaillés.

`producer=4` correspond à `runtime.snapshot` (`type=1`). Son `msg=28` est l'index
28 du registre construit depuis les providers ; sa signification dépend des
routes actives et de leur ordre d'enregistrement. À chaque connexion, les lignes
`route producer=4 type=1 msg=... snapshot=... class=... suffix=...` rendent la
correspondance exacte visible, sans deviner une entité à partir d'un numéro.

### Sémantique actuelle de publication

Le slot est libéré après un retour non négatif de
`esp_mqtt_client_publish()`, sauf si une nouvelle demande impose une réémission.
Le callback `onMessagePublished` signifie donc **acceptation par le client MQTT**,
et non confirmation par le broker. Les messages QoS 1 restent éventuellement
dans l'outbox ESP-MQTT après libération du slot Flow.IO ; cette occupation n'est
pas incluse dans `used`.

Les snapshots runtime et la discovery HA utilisent QoS 0 ; le statut online
utilise QoS 1. Les ACK ont le QoS fourni par leur appelant. Les payloads sont
construits à l'émission dans le buffer central, pas stockés dans chaque job.

### Étapes suivantes : cadence, coalescence et réserve

Ce changement instrumente le comportement existant. Les priorités suivantes
restent à implémenter et à valider à partir des mesures :

1. Ordonnanceur global de démarrage : actuellement le runtime enfile toutes ses
   routes à la connexion ; les producteurs de configuration ont chacun leur
   propre quota (deux demandes par tick, plus une reprise), et HA avance une
   publication à la fois. Aucun quota global ni barrière entre ces phases n'existe.
2. Classification explicite des états remplaçables et événements conservés : la
   déduplication actuelle s'applique à toute clé `(producerId, messageId)`.
   Introduire `messageTypeId/entityId` exige un contrat commun aux producteurs ;
   les alarmes actuelles publient des états, pas un historique des transitions.
   Les ACK utilisent un stockage circulaire qui peut réutiliser une entrée encore
   occupée : une réserve de jobs ne suffirait pas à garantir leur conservation.
3. Réserve critique et équité : dimensionner la réserve avec la charge critique
   réelle, puis protéger l'admission dans le gestionnaire, sans filtrage de topic.

Validation sur cible restant à faire : trois boots (dont un avec réseau retardé
30 s), indisponibilité MQTT de cinq minutes, et saturation artificielle du pool
général. Capturer les snapshots, les compteurs de rejet, les correspondances de
routes et la fin de discovery ; aucun résultat de ces essais matériels n'est
présumé par les tests hôte.
