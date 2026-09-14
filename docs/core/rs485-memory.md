# Allocation mémoire RS485 / Modbus

Les données applicatives volumineuses sont regroupées dans des structures
`Storage` allouées avec `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` :

| Stockage | Contenu | Taille sur ESP32-S3 |
|---|---|---:|
| `ModbusRtuMaster::Storage` | huit transactions et buffers TX/RX | 1 888 octets |
| `Rs485Bus::Storage` | buffer RX applicatif | 256 octets |
| `VariableSpeedPumpModule::Storage` | quatre emplacements de pompe et drivers | 720 octets |

L'allocation est réalisée une seule fois, pendant l'assemblage ou le démarrage,
jamais dans le cycle d'une transaction. Les stockages sont conservés pendant
toute la durée de vie du firmware. `Rs485Bus::end()` arrête le driver UART mais
conserve son stockage applicatif pour permettre un redémarrage sans réallocation.

Il n'existe aucun repli vers la RAM interne. Un échec est journalisé et le
transport ou service concerné reste indisponible. Les méthodes publiques ne
déréférencent pas un stockage absent. Si le démarrage du maître Modbus échoue,
`IOModule` arrête également le driver UART.

Les mutex, objets de contrôle, ressources du driver UART et pile de la tâche
pompe ne sont pas déplacés par cette modification. Les buffers applicatifs ne
sont pas utilisés depuis une interruption ; le driver UART conserve ses propres
buffers et sa gestion des interruptions.

Le contrôle sur carte doit comparer `internal_free` et `largest_internal` avant
et après démarrage, et tester la communication pendant les sauvegardes NVS et
les mises à jour OTA. Le déplacement ne démontre pas à lui seul la cause ni la
résolution d'un crash antérieur.
