# MQTTModule (`moduleId: mqtt`)

## Rôle

Cœur MQTT unifié (transport + scheduling TX):
- connexion broker, souscriptions RX, reconnexion/backoff
- chemin TX unique pour **toutes** les publications MQTT
- files de jobs par priorité (`High`, `Normal`, `Low`)
- build topic/payload **à la demande** dans le buffer central
- dédup/coalescing par clé logique `(producerId, messageId)`
- exposition du service `MqttService`

Le topic du buffer de publication utilise la capacite `DynamicTopic` du profil
carte (192 octets sur Waveshare), distincte des topics fixes de 70 octets. Le
dimensionnement couvre les topics de decouverte Home Assistant, y compris les
longueurs maximales du prefixe, du noeud et de l'identifiant d'objet; un controle
a la compilation dans `HAModule` protege cette limite. Le buffer de publication
reste alloue en PSRAM.

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
- pile du module en RAM interne via `Limits::Mqtt::TaskStackSize` (8 192 octets sur Waveshare)
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

### Admission des publications de fond

`queue background=A/32 deferred=N` (DEBUG, toutes les cinq secondes)
- `background` : jobs de fond admis, y compris construction et attente de retry.
- `deferred` : tentatives différées depuis le boot ; une tentative n'est pas une
  publication perdue. Ce compteur est distinct des rejets immédiats.

`MqttService::tryEnqueueBackground` retourne `Accepted`, `Deferred` ou `Invalid`.
Une demande différée reste chez son producteur, sans slot ni payload dupliqué et
sans expiration après dix secondes. Les anciennes métriques `cfgq` sont retirées.

### Logs IDF MQTT client

Exemple:
`MQTT_CLIENT: mqtt_message_receive: transport_read() error: errno=128`
- erreur transport bas niveau TCP/TLS côté client IDF
- déclenche en pratique une déconnexion puis le cycle de reconnexion/backoff du module

## Synchronisation progressive

La tâche MQTT prépare tous les producteurs (`onTransportTick`), puis parcourt
leurs offres de fond (`onBackgroundTick`) avec un curseur circulaire. Au maximum
une offre acceptée est traitée par cycle. Le curseur avance après cette offre,
ce qui répartit les admissions entre les producteurs actifs.

Le transport limite les publications de fond à `MaxBackgroundJobs = 32`, sous
`jobsMux_`, en vérifiant aussi la capacité du pool et de l'anneau cible. Le crédit
reste occupé pendant les retries et déconnexions, puis revient à la libération du
job. Une coalescence ne consomme pas de crédit supplémentaire. Les ACK, alarmes
et états immédiats d'actionneurs conservent leur admission et leurs priorités.
La limite de fond laisse de la capacité disponible ; elle ne garantit pas les
ACK en cas de saturation du trafic immédiat ou de leur stockage propre.

- Configurations : boîte fixe de 96 demandes avec priorité maximale par route.
  Les événements enregistrent les demandes ; seule la tâche MQTT les retire à
  l'admission. Une demande reçue pendant une admission différée est fusionnée ;
  une demande reçue pendant un job admis survit à sa fin.
- Runtime : la connexion signale la synchronisation à la tâche MQTT. Celle-ci
  marque les routes ; les états numériques sont admis progressivement et les
  actionneurs restent immédiats. Les révisions préservent les changements reçus
  pendant la construction ou l'envoi. Les deadlines périodiques restent liées
  au dernier envoi réussi, et le throttle numérique est vérifié avant admission.
  Le catalogue construit ses routes au démarrage, après la définition des valeurs
  par tous les producteurs. Une préférence de publication activée pour une valeur
  sans producteur ne crée pas de route ni de retry permanent occupant un crédit.
  Les valeurs définies mais non encore mesurées ou invalides conservent leur route
  et publient leur qualité avec une valeur `null`.
- Discovery HA : une seule entité admise à la fois, dans la fenêtre commune.
  Préparation, admission, callbacks et libération des ressources one-shot sont
  exécutés par la tâche MQTT. Les autres tâches signalent les demandes de refresh.

La cadence de transport reste une tentative de publication toutes les 100 ms.
Les buffers et capacités des anneaux restent inchangés.

Validation native :
`python3 -m unittest scripts.tests.test_mqtt_queue scripts.tests.test_mqtt_config_deferred scripts.tests.test_runtime_refresh scripts.tests.test_ha_background scripts.tests.test_data_runtime_routes`.
Les scénarios couvrent 377 publications entre trois producteurs, un transport
bloqué 30 secondes, la reconnexion, les anneaux pleins, les changements pendant
l'envoi et la discovery avec et sans one-shot. Les tests de file utilisent une
fenêtre réduite à trois jobs pour atteindre systématiquement les limites.

## Diagnostic de saturation cohérent

Le diagnostic TX prend un `QueueSnapshot` sous `jobsMux_`, le verrou qui protège
les transitions des jobs, les anneaux et les compteurs d'admission. Les logs sont
émis après déverrouillage. Les lignes `queue at=<millis>` d'un rapport proviennent
du même snapshot ; elles sont séparées pour respecter les 160 caractères de
LogHub. Le rapport périodique est émis toutes les cinq secondes au niveau DEBUG.

Invariants :

- `free + used = MaxJobs` (240 sur Waveshare).
- `used = queued + processing + waiting`.
- `queued = live(high) + live(normal) + live(low)` ; chaque job `Queued` a
  exactement une référence valide dans sa file effective.
- Pour chaque file : `physical = live + stale`.
- `admitted - released = used` (arithmétique des compteurs uint32).
- Le nombre de jobs de classe Background correspond au crédit occupé et reste
  inférieur ou égal à `MaxBackgroundJobs`.

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

### Validation sur cible et limites

Validation sur cible restant à faire : trois boots (dont un avec réseau retardé
30 s), indisponibilité MQTT de cinq minutes, et charge simultanée HTTP/OTA.
Vérifier la fenêtre `background <= 32`, la fin des synchronisations et de la
discovery, ainsi que les compteurs `queue rejects` du trafic immédiat. Aucun
résultat de ces essais matériels n'est présumé par les tests hôte.

L'équité concerne l'admission des publications de fond. Le dispatch conserve ses
priorités existantes : un trafic urgent soutenu peut retarder le fond. Une réserve
critique dédiée et la conservation de chaque réponse de commande demanderaient
un travail distinct sur le stockage circulaire des ACK. La coalescence existante
par `(producerId, messageId)` conserve les états ; elle n'est pas un historique
des occurrences d'événements.
