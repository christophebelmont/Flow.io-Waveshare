# SystemMonitorModule (`moduleId: sysmon`)

## Rôle

Supervision périodique:
- heap, fragmentation, RSSI/IP WiFi
- stack watermark des tasks modules
- résumé périodique des écritures NVS

Type: module actif.

## Dépendances

- `loghub`

## Affinité / cadence

- core: 0
- task: `sysmon`
- loop: delay 200ms
- traces toujours actives (niveau `Info`)
- période traces pilotée par config (`trace_period_ms`, défaut 5000)

## Services consommés

- `wifi` (`WifiService`)
- `config` (`ConfigStoreService`) [référence disponible]
- `loghub`

## Services exposés

Aucun.

## Config / NVS

Module config: `sysmon` (`moduleId = ConfigModuleId::SystemMonitor`, branche locale `1`)
- `trace_period_ms` (`NvsKeys::SystemMonitor::TracePeriodMs`)

## EventBus / DataStore / MQTT

Aucun direct.

## Particularités

- accepte un pointeur `ModuleManager` pour inspecter les stacks (`setModuleManager`)
- baseline stack: à chaque cycle, chaque tâche FreeRTOS est échantillonnée via
  `uxTaskGetStackHighWaterMark()`; le minimum observé depuis le boot est conservé
  par nom de tâche (table PSRAM allouée une fois dans `init`), ce qui survit à la
  recréation d'une tâche et fournit une référence stable pour le redimensionnement.
- format entries: `module/task@cX min=<freeMin>/<stackSize>B` pour les tâches
  modules (taille configurée connue) et `task min=<freeMin>B` pour les autres.
- format résumé: `Stack baseline tasks=<n> low=<n>`; si une tâche atteint un
  watermark nul, `Stack baseline overflow tasks=<n>` est émis en `warn`.
- `!` marque un minimum sous le seuil de sécurité (`512 B`, `1536 B` pour la
  tâche module MQTT) **uniquement pour les tâches dont la taille de stack est
  connue** (tâches modules). Les tâches internes FreeRTOS/ESP-IDF (idle, ipc,
  timer, ...) ont des stacks fixes non redimensionnables: leur minimum est
  affiché mais non signalé, pour éviter de faux positifs.
- au plus `3` tâches par ligne, pour que le relevé reste lisible dans un
  terminal de largeur limitée.
- log des écritures NVS via `ConfigStore::logNvsWriteSummaryIfDue()`

## Baseline stack (relevé de référence)

Séquence de validation: laisser Flow.IO fonctionner environ 1 h en charge avec au
moins une reconnexion réseau, du trafic MQTT, l'ouverture de l'interface Web et
des changements HMI. À la fin:

- chaque tâche doit afficher un `min=` non nul;
- aucune valeur ne doit continuer à chuter fortement entre deux cycles;
- conserver la sortie `Stack baseline ...` comme référence à comparer aux
  prochaines versions avant tout redimensionnement de stack.
