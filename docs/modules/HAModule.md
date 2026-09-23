# HAModule (`moduleId: ha`)

## Rôle

Publication Home Assistant MQTT Discovery:
- registre d'entités statiques (sensor, binary_sensor, switch, number, button)
- publication discovery retainée
- refresh automatique sur changements runtime pertinents
- support du champ discovery `has_entity_name` sur les sensors (piloté par l'entité appelante)

Type: module actif (event-driven par notification task).

## Dépendances

- `eventbus`
- `config`
- `datastore`
- `mqtt`

## Affinité / cadence

- core: 0
- task: `ha`
- loop bloquant sur notification (`ulTaskNotifyTake`)

## Services exposés

- `ha` -> `HAService`
  - `addSensor`, `addBinarySensor`, `addSwitch`, `addNumber`, `addButton`
  - `requestRefresh`
  - `addDiscoveryRemoval` (suppression retained des anciennes découvertes)

## Services consommés

- `eventbus`
- `datastore`
- `mqtt`

## Capacités statiques

Capacités compile-time du profil Waveshare, résolues depuis
`src/Board/WaveshareBoard.h` via `Limits::Ha::Capacity`:

| Type d'entité | Capacité |
|---|---:|
| sensors | 48 |
| binary sensors | 32 |
| switches | 16 |
| numbers | 38 |
| buttons | 32 |
| selects | 14 |

## Config / NVS

Module config: `ha` (`moduleId = ConfigModuleId::Ha`, branche locale `1`):
- `enabled`
- `vendor`
- `device_id` (Identifiant appareil HA)
- `entity_prefix`
- `disc_prefix`
- `model`

Détail `device_id`:
- persistance NVS: `ha_devid`
- si vide: fallback auto basé MAC (hex)
- alimente `unique_id` des entités Discovery
- alimente aussi le segment `<nodeTopicId>` des topics Discovery (`.../<component>/<nodeTopicId>/<objectId>/config`)
- `dev.ids[0]` dans les payloads Discovery suit prioritairement le `deviceId` MQTT effectif (ex: `mq_tid`), puis retombe sur `device_id` si indisponible

## DataStore

Écritures via `HARuntime.h`:
- `HaPublished`
- `HaVendor`
- `HaDeviceId`

## EventBus

Abonnement:
- `DataChanged`

Réactions:
- sur `WifiReady`/`MqttReady` -> tentative/pending de publication auto-discovery

## MQTT

- producteur MQTT enregistré (`producerId` dédié HA) sur le cœur TX unifié
- jobs HA en mode IDs (`producerId + messageId`), sans publication directe module -> broker
- build topic/payload discovery à la demande via `MqttBuildContext` (buffer central MQTT)
- construit topics discovery:
  - `<discoveryPrefix>/<component>/<nodeTopicId>/<objectId>/config`
- préfixe `object_id` configurable:
  - défaut `fio_*` (via `entity_prefix=fio`)
  - si `entity_prefix` est non vide: `object_id = <prefix>_<entity>`
  - si `entity_prefix` est vide: `object_id = <entity>` (sans préfixe)
- payloads incluent:
  - device metadata
  - `device.name` prioritairement depuis `mqtt.deviceName` (fallback: nom d'origine du profil)
  - `device.identifiers[0] = <vendor>-<mqttDeviceIdEffectif>` (priorité à `mqtt.topicDeviceId` / `mq_tid`; fallback sur `ha/device_id`)
  - `unique_id` construit avec `ha/device_id` (ou fallback MAC hex si vide)
  - le segment `<nodeTopicId>` du topic Discovery est dérivé de `ha/device_id` (ou fallback MAC hex si vide)
  - `availability` basée sur topic `status`
- capteurs diagnostic natifs publiés:
  - `alarms_pack` (`rt/alarms/p`)
  - `uptime` (`rt/system/state`, conversion en minutes depuis `upt_ms`)
  - `heap_free_bytes` (`rt/system/state`, conversion en `ko`)
  - `heap_min_free_bytes` (`rt/system/state`, conversion en `ko`)
  - `heap_fragmentation` (`rt/system/state`, valeur `%`)

## Comportement refresh

- `add*` met à jour la table d'entités et demande refresh
- `requestRefresh` remet `published=false` et notifie la task
- publication effective seulement si MQTT connecté et `mqttReady(DataStore)==true`

## Mode one-shot

Le build peut définir `FLOW_HA_ONESHOT_DISCOVERY=1` pour publier l'auto-discovery une seule fois au démarrage.
Ce mode est utilisé par le firmware Waveshare:
- les tables d'entités HA sont allouées dynamiquement au lieu d'être conservées en `.bss`
- le producteur MQTT de configuration HA n'est pas instancié
- après publication retained de toutes les entités discovery, les tables sont libérées et la tâche `ha` appelle `vTaskDelete(nullptr)`
- le service HA reste présent mais refuse les nouveaux enregistrements après teardown, afin d'éviter des pointeurs pendants dans les services/callbacks existants
- pour diagnostiquer la séquence de boot one-shot, le build peut activer `FLOW_HA_BOOT_TRACE=1` (logs de jalons alloc/enqueue/publish/release)

## Alarmes natives

`AlarmModule` génère la découverte depuis son registre après l'initialisation de
l'ensemble des modules et avant le démarrage des tâches. Aucun template YAML
Home Assistant n'est nécessaire.

Pour chaque alarme enregistrée:
- `binary_sensor.<prefix>_alm_<AlarmId>_active`, avec classe `problem`;
- `button.<prefix>_alm_<AlarmId>_reset`, commande `alarms.reset` avec `args.id`;
- attributs du capteur: `alarm_id`, `slot`, `resettable`, `latch_enabled`,
  `condition` (`true`, `false`, `unknown`), `severity` (valeur numérique).

Ces nouvelles identités n'incluent pas le nom affiché: un changement de titre
ou d'ordre des slots ne change pas la cible de l'entité. Les entités existantes
conservent leur convention d'identité historique.

Le bouton individuel est disponible seulement si Flow.IO est en ligne et
`r == 1`. Les alarmes automatiques ont également un bouton, mais il reste
indisponible. Le moteur conserve la validation autoritaire du reset.

Entités globales:
- `alm_any_active`: au moins une alarme active;
- `alm_active_count`: nombre d'alarmes actives;
- `alm_reset_all`: bouton historique conservé, disponible si au moins une
  alarme est acquittable et si Flow.IO est en ligne;
- `alm_pack`: diagnostic historique conservé pour les consommateurs existants.

Les états MQTT des alarmes sont retained et resynchronisés à chaque connexion
Flow.IO au broker. Une reconnexion de Home Assistant récupère ainsi les derniers
états sans attendre une transition. La disponibilité globale reste liée au LWT.

Les anciennes découvertes `alm_reset_slot_0` à `alm_reset_slot_7` sont supprimées
par publication vide retained, après enregistrement des nouvelles entités. Les
helpers YAML installés manuellement ne sont pas supprimés: leurs utilisateurs
peuvent migrer leurs cartes/automatisations puis retirer l'ancien include.

### Contrat des boutons

`HAButtonEntry.payloadPress` contient toujours le payload MQTT brut. Le
sérialiseur ArduinoJson réalise l'échappement JSON de la découverte; les modules
ne doivent pas fournir de JSON pré-échappé. Les commandes de boutons ne sont
jamais retained. `availabilityTopicSuffix` et `availabilityTemplate` ajoutent une
condition de disponibilité combinée au statut de l'appareil (`all`).

Les capteurs binaires peuvent transmettre `attributesTemplate` pour extraire
leurs attributs depuis le même topic d'état. Les documents temporaires de ces
deux constructeurs sont alloués en PSRAM; un débordement échoue explicitement.
