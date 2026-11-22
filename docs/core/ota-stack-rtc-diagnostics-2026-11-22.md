# Flow.io — diagnostic OTA / pile / redémarrage (22 novembre 2026)

## Origine

Sur le banc de laboratoire, deux cycles complets d'OTA PSRAM A/B ont réussi.
Le journal `device-monitor-261122-114031.log` montre toutefois
`[OTA-MEM] phase=paired_manifest_json_psram stack_free=28` et
`release_manifest_json_psram stack_free=2`, ainsi que
`[BOOT-RESET] reset_reason=3 rtc_valid=0 planned=none` malgré des
redémarrages OTA prévus.

Les deux équipements RS485 sont **absents du banc** : leurs timeouts
ne sont pas des défauts du firmware. Lors de ces essais, `enabled=000`
pour les ports TCP : aucun stress multi-port n'était validé.

## Changements du patch

- Le worker `fwupdate` conserve une pile **interne**, mais passe de
  8 192 à **10 240 octets** (coût supplémentaire de 2 048 o en DRAM).
  Il ne s'agit pas d'une optimisation mémoire.
- La première analyse ArduinoJson conserve son pool en PSRAM et est
  détruite AVANT l'appel de validation imbriqué `beginLocalRelease_`.
  On ne change ni URLs, ni SHA-256, ni transaction A/B.
- Chaque `[OTA-MEM]` identifie le nom et l'adresse de la tâche, et
  le minimum `stack_hwm` en octets pour mieux diagnostiquer les
  valeurs anormales (nouveau `[OTA-STACK] LOW` sous 1024 octets).
- `RebootDiagnostics::noteOta` reste **RTC uniquement** : zéro écriture
  NVS sur les milliers de fragments HTTP ou les écritures Flash.
  `markPlanned` effectue UNE sauvegarde de l'intention de redémarrage
  dans un espace NVS distinct `flowio_boot/planned`, juste avant
  `ESP.restart()`. Au prochain démarrage le marqueur NVS est lu,
  vérifié (magic/version/checksum) et effacé. S'il n'existe pas de
  RTC valide et que le reset était logiciel (`ESP_RST_SW`), le champ
  `source=nvs` rétablit l'intention OTA.
  Un reset inattendu ou une coupure d'alimentation ne doit pas être
  présenté comme redémarrage OTA planifié, même si NVS contient
  un marqueur obsolète.
- Un échec de sauvegarde NVS est journalisé avec `nvs_saved=0` :
  l'OTA ne dépend **pas** de cette télémétrie.

## Tests

Tests source statiques : 
`node scripts/tests/check_ota_reboot_diags.mjs`
et
`node scripts/tests/check_psram_ota_placement.mjs`.
Il faut encore compiler sous PlatformIO, tester le maintien des
deux redémarrages et vérifier le comportement RTC/NVS sur le matériel.
Sur le reboot suivant, chercher
`[BOOT-RESET] reset_reason=3 ... nvs_valid=1 source=nvs planned=ota_release`
si RTC reste perdu. Selon les plate-formes, `source=rtc` est aussi valide.

Contrôler enfin `[OTA-MEM] ... task=fwupdate ... stack_hwm=...`,
`[OTA-STACK] LOW`, le plus grand bloc libre interne et le bilan
PSRAM avec 0, 1, 2 et 3 ports TCP actifs.

## Isolation

Modifie uniquement `experiment/modbus-tcp-psram-buffers-20261122`
(PR #2). La branche `experiment/ota-psram-preverify-20261121`
reste intacte. La nouvelle marge de pile coûte 2 Ko, compensés
théoriquement par le déplacement de 3 120 o de buffers Modbus TCP.
