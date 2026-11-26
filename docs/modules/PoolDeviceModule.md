# PoolDeviceModule

Gestionnaire métier des équipements de piscine, indépendant du raccordement physique.

L'architecture, les paramètres de pilotes, les nouvelles clés NVS et les exemples sont décrits dans [Pilotage des équipements](PoolActuators.md).

Le service `PoolDeviceService` expose `meta`, `setRunning`, `setTarget`, `readState`, `readActualOn`, `setWritesEnabled`, `writesEnabled` et `refillTank`. Les commandes passent toutes par les dépendances, l'activation et les limites journalières. Un acquittement de commande ne constitue pas une mesure physique.

Les [modes Auto / Manu / Hiver](../integration/poollogic-operating-modes.md)
sont définis par PoolLogic. Les commandes directes des appareils affectés à un
rôle PoolLogic nécessitent Manu ; en Auto/Hiver, utiliser un forçage temporaire
compatible. Cette autorisation est vérifiée dans `setTarget_` pour toutes les
sources de commande. Les consignes restent éditables et les types de traitement
ne sont jamais modifiés par une commande directe.

Les métriques comprennent temps de fonctionnement, volumes estimés et niveau de cuve. Les événements du scheduler réinitialisent les périodes jour/semaine/mois ; les changements d'heure déclenchent leur réconciliation. La persistance runtime est effectuée à l'arrêt, sur remise à zéro/remplissage ou toutes les 60 secondes pendant le fonctionnement.

Le profil Waveshare expose 16 appareils, `pd0` à `pd15`. Les huit premiers conservent leurs préréglages : filtration, pH, chlore, robot, remplissage, électrolyse, lumières, chauffage. Les suivants sont des relais génériques `PD08` à `PD15`. Le descripteur de commande décide des sorties ou de l'adresse série ; il n'impose plus `pdN == dN`. Les dépendances utilisent un masque 16 bits.

Les rôles automatiques sont affectés dans `poollogic/devices`. La découverte HA des rôles utilise ces affectations au démarrage, y compris pour les compteurs, paramètres et boutons associés. Les identifiants HA historiques sont conservés ; les entités de consigne propres aux pilotes restent attachées à leur appareil.

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

## Capacités pour 16 appareils

Le registre de commandes réserve 80 entrées, afin de conserver les commandes des autres modules après l’enregistrement des 16 appareils. Le registre de configuration réserve 896 entrées (métadonnées en PSRAM lorsque disponible). Les avertissements de saturation restent actifs.
