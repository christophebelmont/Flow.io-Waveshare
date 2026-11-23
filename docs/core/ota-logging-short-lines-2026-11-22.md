# Flow.io — journaux OTA et reboot sans troncature (22 novembre 2026)

## Cause confirmée

Le `LogEntry.msg` central est limité à `LOG_MSG_MAX=160`,
soit **159 caractères utiles** dans `src/Core/Services/ILogger.h`.
`Log.cpp` tronque au moment du `vsnprintf`, **avant** le WebSocket
et le téléchargement du journal. Dans les captures labo, la ligne
`[OTA-MEM] ... stack_free=4` était tronquée ; on ne doit donc
pas l'interpréter comme seulement 4 octets de pile libre.

La tâche `fwupdate` est déjà configurée avec 10 240 octets de pile
interne. Les impressions directes du même worker indiquaient
4 868 puis 4 536 octets à des points critiques.

## Correction

`otaMonitor_` produit désormais deux événements bornés :

- `[OTA-MEM] phase=... task=fwupdate id=0x... core=0 hwm_bytes=... internal=... largest=...`
- `[OTA-HEAP] phase=... ms=... min_internal=... frag_pct=... psram=... largest_psram=...`

Le champ `hwm_bytes` arrive suffisamment tôt pour être visible
même si un nom imprévu est trop long. Le second événement donne
la mémoire minimale interne et le plus grand bloc PSRAM.
Les deux lignes conservent le `phase` pour leur corrélation.

Le redémarrage émet deux lignes `[BOOT-RESET]` supplémentaires dans
le **LogHub** (donc bootlogs Web) quand le hub est disponible, et
conserve intégralement la ligne `Serial.printf` sur l'USB/UART.
L'absence de LogHub en début de démarrage laisse intact le
diagnostic série historique.

Ce patch ne change ni la taille de `LogEntry` (qui aurait augmenté
les mémoires tampon de journalisation), ni la pile OTA, ni la
logique PSRAM/Flash/SHA, ni le mécanisme RTC/NVS.
`[MODBUS-PSRAM] ready` ne peut apparaître que lorsque le
serveur Modbus TCP est effectivement démarré.

## Validation

- `node scripts/tests/check_ota_log_line_lengths.mjs` : les quatre
  nouvelles lignes les plus critiques sont vérifiées sous la limite
  réelle de LogHub avec des longueurs de valeurs pessimistes.
- `node scripts/tests/check_psram_ota_placement.mjs` et
  `node scripts/tests/check_ota_reboot_diags.mjs` restent valides.
- Sur ESP32-S3 : vérifier des `hwm_bytes` entiers au cours des deux
  étapes `spiffs_progress` / `firmware_progress`, et les deux
  lignes `[BOOT-RESET]` après reboot A/B.
- Contrôler les statistiques `formatTruncCount` si disponibles.
- Tester OTA avec ports Modbus TCP activés séparément :
  ces essais ne sont pas couverts par les seuls journaux du labo.

Le correctif est appliqué directement sur
`experiment/modbus-tcp-psram-buffers-20261122`, en conservant la PR #2.
