# PoolDeviceModule

Gestionnaire métier des équipements de piscine, indépendant du raccordement physique.

L'architecture, les paramètres de pilotes, les nouvelles clés NVS et les exemples sont décrits dans [Pilotage des équipements](PoolActuators.md).

Le service `PoolDeviceService` expose `meta`, `setRunning`, `setTarget`, `readState`, `readActualOn`, `setWritesEnabled`, `writesEnabled` et `refillTank`. Les commandes passent toutes par les dépendances, l'activation et les limites journalières. Un acquittement de commande ne constitue pas une mesure physique.

Les métriques comprennent temps de fonctionnement, volumes estimés et niveau de cuve. Les événements du scheduler réinitialisent les périodes jour/semaine/mois ; les changements d'heure déclenchent leur réconciliation. La persistance runtime est effectuée à l'arrêt, sur remise à zéro/remplissage ou toutes les 60 secondes pendant le fonctionnement.

Les slots du profil Waveshare conservent leurs identités métier : filtration, pH, chlore, robot, remplissage, électrolyse, lumières, chauffage. Le descripteur de commande décide des sorties ou de l'adresse série ; il n'impose plus `pdN == dN`.

### Interlock availability and operation diagnostics

An unsatisfied dependency alone does not create a device fault. The runtime and
I/O summary expose `interlock_state`: `0` ready, `1` unavailable without a blocked
operation, `2` start rejected, `3` safety stop. The I/O summary also exposes the
numeric `block_code`, so presentation does not infer semantics from error text.
A stopped, unavailable device has no interlock error and appears as stopped;
its status tooltip explains that a dependency prevents starting.

A rejected start is published immediately. Losing a dependency while a target
is requested, applied, or observed running cancels the target and records a
safety stop. These diagnostics survive subsequent idle ticks until dependencies
recover or an accepted stop acknowledges them (a still-running device can raise
the safety stop again). Recovery never restores the cancelled target; a new
start command is required. Driver faults continue to take precedence over the
interlock diagnostic, including when no start was requested.
