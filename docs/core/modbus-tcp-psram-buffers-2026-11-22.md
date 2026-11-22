# Modbus TCP — déplacement des buffers RX/TX en PSRAM (22 novembre 2026)

## Branche et périmètre
Base : `experiment/ota-psram-preverify-20261121` à `46b87a4a1a303a57852f2a5bfbf18b394a6510d2`. Branche de validation : `experiment/modbus-tcp-psram-buffers-20261122`.
Seuls les tableaux de six ADU entrants et de six ADU sortants sont déplacés.
Les sockets, états, temporisations, pilotes RS485 et priorités de sécurité
restent inchangés. Les ports 502, 503, 504 conservent deux clients chacun.

## Budget
- RX : 6 × 260 = **1 560 octets**.
- TX : 6 × 260 = **1 560 octets**.
- Total déplacé : **3 120 octets** de buffers précédemment internes.

Ce gain est théorique et concerne la mémoire statique du programme. Le relevé
du tas interne juste avant et après l'allocation PSRAM ne mesure pas la
diminution de la section .bss entre deux binaires.

## Mécanisme
Le serveur alloue une fois un `FrameStorage` par
`heap_caps_calloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.
Il transmet à `TcpSessions` les six buffers RX et à chaque `BridgeSlot`
un buffer TX, sans modifier leur état de contrôle (RAM interne).
Il conserve le bloc durant toutes les reconnexions Wi-Fi.
En cas d'échec, il **n'ouvre aucun listener**, ne retombe pas en DRAM
et réessaie au maximum toutes les 10 secondes. Diagnostic
`[MODBUS-PSRAM] allocation_failed`.
Au succès : `[MODBUS-PSRAM] ready rx_bytes=1560 tx_bytes=1560 total=3120`.
La ligne périodique `[MODBUS-PSRAM] active_frame_bytes=3120 ready=1`
confirme le pool actif.

## Test / validation
1. Relever sur la branche de base les valeurs `heap_internal`,
   `largest_internal`, `psram`, `stack_hwm` et `tick_max_us`.
2. Compiler cette branche expérimentale, comparer avec les **mêmes** services,
   conditions réseau et configurations.
3. Tester six sockets persistantes, fragmentation TCP et reconnexions ;
   aucun tableau ne doit être réalloué lors de `stop()` / `begin()`.
4. Vérifier FC03/FC04 sur 125 registres ; FC06/FC16 sur registres de test
   **sans effet dangereux** et exceptions Modbus / délais.
5. Refaire les essais OTA avec 0, 1, 2, 3 ports TCP actifs, inspecter
   `[OTA-NET]`, `[MODBUS-TCP]`, latences RS485 et temps de réponse.
6. Lancer `node scripts/tests/check_modbus_tcp_psram_buffers.mjs`
   ainsi que les tests hôtes `test/host/modbus_tcp_sessions.cpp`,
   `test/host/modbus_tcp_bridge.cpp` et le build PlatformIO.

Ce patch n'est pas une validation sur matériel. Les buffers lwIP,
piles de tâches et mémoire UART demeurent internes. Une indisponibilité
PSRAM rend les listeners Modbus TCP indisponibles (mode fail-closed).

## Retour arrière
Revenir à `experiment/ota-psram-preverify-20261121`, sans changement de NVS ni partitions.
