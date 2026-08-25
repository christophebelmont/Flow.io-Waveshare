# LogHubModule (`moduleId: loghub`)

## Rôle

Point d'entrée central du logging asynchrone.
- héberge `LogHub` (queue de logs)
- héberge le registre de sinks (`LogSinkRegistry`)
- expose les services de log pour tous les modules

Type: module passif (`ModulePassive`, pas de task).

## Dépendances

Aucune dépendance déclarée.

## Services exposés

- `loghub` -> `LogHubService`
- `logsinks` -> `LogSinkRegistryService`

Le module appelle aussi `Log::setHub(&hubSvc)` pour activer les helpers globaux `Log::info/debug/...`.

## Config / NVS

Le module expose dynamiquement une configuration de niveau minimum par module:
- branche config: `log/levels`
- clé JSON par module: `m<ID>_lvl`
- clé NVS par module: `lgm<ID>` (format `lgm%03u`)
- niveaux: `0=Debug`, `1=Info`, `2=Warn`, `3=Error`

Ces variables sont créées au moment où un module est enregistré dans le registre de logs.

## Table LogModuleId

Mapping officiel `ID -> tag` utilisé par le registre de logs (`src/Core/LogModuleIds.h`):

| ID | Constante | Tag / nom affiché |
|---:|---|---|
| 0 | `Unknown` | `(non défini)` |
| 1 | `Core` | `core` |
| 2 | `LogHub` | `loghub` |
| 3 | `LogDispatcher` | `log.dispatcher` |
| 4 | `LogSinkSerial` | `log.sink.serial` |
| 5 | `LogSinkAlarm` | `log.sink.alarm` |
| 6 | `EventBusModule` | `eventbus` |
| 7 | `ConfigStoreModule` | `config` |
| 8 | `DataStoreModule` | `datastore` |
| 9 | `CommandModule` | `cmd` |
| 10 | `AlarmModule` | `alarms` |
| 11 | `WifiModule` | `wifi` |
| 12 | `WifiProvisioningModule` | `wifiprov` |
| 13 | `TimeModule` | `time` |
| 14 | `I2cCfgClientModule` | `i2ccfg.client` |
| 15 | `I2cCfgServerModule` | `i2ccfg.server` |
| 16 | `WebInterfaceModule` | `webinterface` |
| 17 | `FirmwareUpdateModule` | `fwupdate` |
| 18 | `SystemModule` | `system` |
| 19 | `SystemMonitorModule` | `sysmon` |
| 20 | `HAModule` | `ha` |
| 21 | `MQTTModule` | `mqtt` |
| 22 | `IOModule` | `io` |
| 23 | `PoolDeviceModule` | `pooldev` |
| 24 | `PoolLogicModule` | `poollogic` |
| 25 | `HMIModule` | `hmi` |
| 40 | `CoreI2cLink` | `core.i2clink` |
| 41 | `CoreModuleManager` | `core.modulemanager` |
| 42 | `CoreConfigStore` | `core.configstore` |
| 43 | `CoreEventBus` | `core.eventbus` |

## EventBus / DataStore / MQTT

- aucun abonnement EventBus
- aucune publication EventBus
- aucune écriture DataStore
- aucune publication MQTT directe

## Interfaces complémentaires

- ajouter un nouveau sink via `logsinks.add(...)`
- ajuster la profondeur de queue via `Limits::LogQueueLen`
