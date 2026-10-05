# IOModule (`moduleId: io`)

## Rôle

`IOModule` est la couche d'entrées/sorties du firmware principal Waveshare.

Le module regroupe actuellement:

- l'inventaire des endpoints IO exposés aux autres modules
- les drivers GPIO, ADS1115, DS18B20, TCA9554, MCP23017 et PCF8574
- le transport RS485 et le maître Modbus RTU partagés par les modules d'équipements
- le scheduler d'acquisition
- la persistance et l'application de la configuration IO
- les snapshots runtime pour MQTT
- les valeurs Runtime UI liées aux mesures principales
- la synchronisation Home Assistant des capteurs et sorties déclarés par le profil

Les services publics exposés au reste du firmware sont `IOServiceV2` et
`ModbusMasterService`.

La cartographie complète du profil Waveshare est documentée dans
[`docs/core/waveshare-io-map.md`](../core/waveshare-io-map.md), avec les tables
`binding_port`, `io_slot` et `domain_slot`.

Type: module actif.

## Dépendances

En build Waveshare, les dépendances déclarées sont:

- `loghub`
- `datastore`
- `mqtt`

Le profil Waveshare lui associe en plus:

- les bus 1-Wire `oneWireWater` et `oneWireAir`
- la table de ports physiques définie dans `src/Profiles/Waveshare/WaveshareIoLayout.h`
- les définitions d'endpoints construites dans `src/Profiles/Waveshare/WaveshareIoAssembly.cpp`

## Affinité et cadence

- core: `1`
- task: `io`
- stack: `4096` octets (inclut la marge pour le formatage flottant des traces analogiques)
- boucle: `10 ms`

Jobs planifiés en interne:

- `ads_fast`
- `ds_slow`
- `din_poll`

## Services exposés

- `io` -> `IOServiceV2`
- `modbus_master` -> `ModbusMasterService`

Fonctions principales de `IOServiceV2`:

- inventaire des endpoints: `count`, `idAt`, `meta`
- lecture générique: `readValue`
- lecture digitale: `readDigital`
- écriture digitale: `writeDigital`
- lecture analogique: `readAnalog`
- suivi du cycle IO: `tick`, `lastCycle`

`ModbusMasterService` propose des transactions asynchrones pour les fonctions
Modbus RTU `0x03`, `0x04`, `0x06` et `0x10`. Les modules consommateurs
soumettent une requête puis interrogent son résultat avec l'identifiant de
transaction retourné. Le détail du transport, de la file et des erreurs est
décrit dans la [documentation RS485 / Modbus](ModbusMaster.md).

## Capacités statiques

Capacités compile-time actuelles dans `src/Modules/IOModule/IOModule.h`:

| Capacité | Valeur |
|---|---:|
| entrées analogiques | 21 |
| entrées digitales | 13 |
| sorties digitales | 16 |
| slots de configuration correspondants | 16 / 13 / 16 |

Plages d'`IoId` utilisées:

- sorties digitales: `IO_ID_DO_BASE .. IO_ID_DO_BASE + 15`
- entrées digitales: `IO_ID_DI_BASE .. IO_ID_DI_BASE + 12`
- entrées analogiques: `IO_ID_AI_BASE .. IO_ID_AI_BASE + 15`

## Backends physiques pris en charge

Backends actuellement gérés par le module:

- GPIO direct
- sorties via `PCF8574`
- entrées analogiques `ADS1115`
- sondes `DS18B20`
- capteurs `SHT40`, `BMP280`, `BME680`
- mesures électriques `INA226`
- compteur d'impulsions sur GPIO avec debounce

Les `IoBackend` visibles dans le service sont:

- `IO_BACKEND_GPIO`
- `IO_BACKEND_PCF8574`
- `IO_BACKEND_ADS1115_INT`
- `IO_BACKEND_ADS1115_EXT_DIFF`
- `IO_BACKEND_DS18B20`
- `IO_BACKEND_SHT40`
- `IO_BACKEND_BMP280`
- `IO_BACKEND_BME680`
- `IO_BACKEND_INA226`

## Adresses I2C primaire et secondaire

Chaque driver I2C configurable du module IO (`ads1115_int`, `ads1115_ext`,
`sht40`, `bmp280`, `bme680`, `ina226`, `expander00..03`) expose deux champs
persistants dans l'interface de configuration :

- `address` : adresse primaire ; la clé NVS existante est conservée.
- `secondary_address` : adresse de secours ; `0x00` désactive le secours.

L'INA226 utilise par défaut `0x40` puis `0x44`. Les autres secondaires sont
initialement désactivées. Les changements prennent effet au redémarrage.
Les adresses non nulles doivent être comprises entre `0x08` et `0x77` et être
compatibles avec le composant. Le driver SHT40 actuel accepte uniquement `0x44`.

Avant toute initialisation des drivers, une résolution commune réserve les
primaires de tous les drivers activés, même sans binding ou sans réponse du
composant. Les ADS1115, sans interrupteur d'activation dédié, sont activés par
leurs bindings. Les drivers désactivés ne réservent aucune adresse.

La primaire est choisie si elle répond. Sinon, la secondaire peut être choisie
si elle répond et n'est ni une primaire réservée ni demandée par un autre
driver ayant besoin de son secours. Deux primaires identiques ou deux secours
concurrents sont refusés pour tous les participants concernés, avec diagnostic
dans les logs. L'ordre d'initialisation ne décide jamais du propriétaire.
Une erreur de configuration d'adresse désactive l'attribution pour ce driver.

Ainsi, avec INA226 `0x40/0x44` et SHT40 `0x44/0x00`, un SHT40 activé réserve
`0x44` et empêche le secours INA226. S'il est désactivé, l'INA226 peut utiliser
`0x44` lorsque `0x40` ne répond pas. Si les deux adresses répondent, l'INA226
reste à `0x40` et le SHT40 activé utilise `0x44`.

L'adresse retenue est uniquement un état runtime : elle ne remplace jamais la
configuration persistante et tous les échanges suivants l'utilisent. Un échec
d'initialisation du composant ne libère pas sa réservation et ne provoque pas
une réattribution à un autre driver. L'INA226 vérifie son identité avant les
écritures de configuration et borne l'attente de première conversion à 150 ms.
Un acquittement du bus ne constitue pas une identification de composant.

Cette résolution concerne les drivers du module IO. Les périphériques internes
à adresse fixe des autres modules (RTC et panneau de LED HMI) conservent leur
configuration matérielle ; leurs adresses ne doivent pas être attribuées à un
driver IO. Deux composants physiques partageant une adresse sur le même bus
nécessitent toujours une correction du câblage ou de l'adressage matériel.

## Modèle de binding actuel

Le module ne déduit pas seul le câblage métier. Le binding est fourni par le profil Waveshare.

### Catalogue des ports physiques

`src/Profiles/Waveshare/WaveshareIoLayout.h` déclare:

- les `PhysicalPortId`
- la table `kBindingPorts`
- les bindings par défaut des rôles analogiques
- les bindings par défaut des entrées digitales
- les bindings par défaut des sorties digitales

Ports déclarés actuellement:

- ADS interne: `PortAdsInternal0..3`
- ADS externe différentiel: `PortAdsExternal0..1`
- DS18B20: `PortOneWireWater`, `PortOneWireAir`
- SHT40: `PortSht40Temp`, `PortSht40Humidity`
- BMP280: `PortBmp280Temp`, `PortBmp280Pressure`
- BME688 via driver BME680: `PortBme688Temp`, `PortBme688Humidity`, `PortBme688Pressure`, `PortBme688Gas`
- INA226: `PortIna226ShuntMv`, `PortIna226BusV`, `PortIna226CurrentMa`, `PortIna226PowerMw`, `PortIna226LoadV`
- entrées digitales intégrées: `PortGpio5Input..PortGpio11Input` (GPIO4 est réservé au reset système)
- entrées d'extension: `PortMcpInGpa0..6`
- sorties relais TCA9554: `PortExio1..8`
- sorties d'extension: `PortMcpOutGpb0..7`
- GPIO génériques déclarés pour les builds sans TFT

### Instanciation des endpoints

`src/Profiles/Waveshare/WaveshareIoAssembly.cpp` lit le domaine actif et appelle:

- `defineAnalogInput`
- `defineDigitalInput`
- `defineDigitalOutput`

Le profil Waveshare instancie aujourd'hui:

- 21 entrées analogiques (`a00..a20`)
- 13 entrées digitales (`i00..i12`)
- 16 sorties digitales (`d00..d15`)

## Affectation actuelle des rôles Waveshare

### Entrées analogiques

| Rôle par défaut | Port physique par défaut |
|---|---|
| `SensorOrp` | `PortAdsInternal0` |
| `SensorPh` | `PortAdsInternal1` |
| `SensorPsi` | `PortAdsInternal2` |
| `SensorSpareAnalog` | `PortAdsInternal3` |
| `SensorWaterTemp` | `PortOneWireWater` |
| `SensorAirTemp` | `PortOneWireAir` |
| `SensorCurrent` | `PortIna226CurrentMa` |
| `SensorVoltage` | `PortIna226BusV` |

### Entrées digitales

| Rôle par défaut | Port physique par défaut | Mode |
|---|---|---|
| `SensorPir` (`i07`) | `PortGpio11Input` | état |
| `SensorPhLevel` (`i04`) | `PortGpio8Input` | état |
| `SensorChlorineLevel` (`i03`) | `PortGpio7Input` | état |
| `SensorPoolLevel` (`i02`) | `PortGpio6Input` | état |
| `SensorWaterMeter` (`i01`) | `PortGpio5Input` | compteur, actif bas (`actif=0`), front montant, debounce `100000 us` |

### Sorties digitales

| Rôle par défaut | Port physique par défaut |
|---|---|
| `ActuatorFiltrationPump` | `PortExio1` |
| `ActuatorPhPump` | `PortExio2` |
| `ActuatorChlorinePump` | `PortExio3` |
| `ActuatorRobot` | `PortExio4` |
| `ActuatorFillPump` | `PortExio5` |
| `ActuatorChlorineGenerator` | `PortExio6` |
| éclairage `d06` (`Lights`) | `PortExio7` |
| `ActuatorWaterHeater` | `PortExio8` |

## Configuration et NVS

Structures de configuration utilisées:

- `IOModuleConfig`
- `IOAnalogSlotConfig`
- `IODigitalInputSlotConfig`
- `IODigitalOutputSlotConfig`

Branches de configuration exposées par le module:

- `Io`
- `IoDebug`
- `IoInputA0 .. IoInputA5`
- `IoOutputD0 .. D7`
- `IoInputD0 .. D3`

Paramètres principaux:

- activation module: `enabled`
- initialisation du bus I2C principal partagé: `i2c_sda`, `i2c_scl`
- polling: `ads_poll_ms`, `ds_poll_ms`, `digital_poll_ms`
- ADS: adresses, gain, rate
- expanders PCF8574/TCA9554/MCP23017: activation, adresse et masque par instance `expanderXX`
- inversion électrique éventuelle des sorties d'expander: propriété fixe du profil matériel, non configurable
- traces: `trace_enabled`, `trace_period_ms`
- calibration et précision des entrées analogiques
- binding, `activeHigh`, `momentary`, `pulseMs` des sorties digitales
- binding, `pullMode`, `edgeMode`, debounce des entrées digitales

### Sémantique `activeHigh` / `edgeMode` pour les compteurs GPIO

Pour les entrées digitales en mode compteur, le driver ne raisonne pas directement en "front physique brut sur le pin".
Il convertit d'abord le niveau lu en un état logique `logicalOn`:

- `activeHigh=true`:
  `HIGH` = actif, `LOW` = inactif
- `activeHigh=false`:
  `LOW` = actif, `HIGH` = inactif

Le champ `edgeMode` est ensuite appliqué sur cette transition logique:

- `rising`: passage logique `inactif -> actif`
- `falling`: passage logique `actif -> inactif`
- `both`: les deux transitions logiques

Conséquence importante:
sur une entrée active bas (`activeHigh=false`), le front logique montant correspond physiquement à un front descendant sur le pin, et le front logique descendant correspond physiquement à un front montant sur le pin.

Tableau complet du comportement actuel du driver compteur GPIO:

| `activeHigh` | `edgeMode` | Interruption physique armée sur le pin | Transition logique effectivement comptée | Interprétation pratique |
|---|---|---|---|---|
| `true` | `falling` | front descendant | `actif -> inactif` | fin d'une impulsion active haut |
| `true` | `rising` | front montant | `inactif -> actif` | début d'une impulsion active haut |
| `true` | `both` | les deux fronts | les deux transitions | compte montée + descente |
| `false` | `falling` | front montant | `actif -> inactif` | fin d'une impulsion active bas |
| `false` | `rising` | front descendant | `inactif -> actif` | début d'une impulsion active bas |
| `false` | `both` | les deux fronts | les deux transitions | compte descente + remontée |

Exemples pratiques:

- capteur avec pull-up externe et impulsion active bas:
  `activeHigh=false`, `edgeMode=rising` comptera le début de l'impulsion, donc le front physique descendant
- capteur avec pull-up externe et impulsion active bas:
  `activeHigh=false`, `edgeMode=falling` comptera la fin de l'impulsion, donc le front physique montant
- capteur avec impulsion active haut:
  `activeHigh=true`, `edgeMode=rising` comptera le front physique montant

Cette convention est cohérente avec une interprétation "métier" de `edgeMode` dans le domaine logique du signal.
Elle peut toutefois surprendre si l'on s'attend à ce que `rising` et `falling` désignent toujours les fronts physiques bruts du GPIO.

### Persistance NVS du cumul compteur

Le compte courant est conservé en RAM dans `DigitalSlot::pulse.count` (entier 64 bits), puis publié dans le DataStore. Le total converti applique le coefficient `c0` à ce compte.

`IOPulsePersistence.cpp` sauvegarde les comptes, générations et jetons de remise à zéro dans le blob NVS `pulse_v1`, via les `Preferences` du `ConfigStore`. Malgré le nom `writeRuntimeBlob`, cette sauvegarde utilise la partition NVS, pas la partition distincte `runtime`.

- Une sauvegarde asynchrone est demandée toutes les heures si les données ont changé (`PulseCheckpoint::PeriodMs = 3600000`). Après un échec, une nouvelle tentative est prévue après une minute.
- Une demande de reboot (`system.reboot`, redémarrage après provisioning) attend la confirmation d'une sauvegarde immédiate. Les demandes d'OTA distante et de release locale font de même avant de commencer, puis une nouvelle sauvegarde est confirmée avant le redémarrage final pour inclure les impulsions reçues pendant la mise à jour.
- La tâche IO relit les compteurs matériels et transmet le snapshot à la tâche de persistance. Les appelants attendent au maximum deux secondes. Une erreur de lecture, d'écriture, un délai dépassé ou une opération concurrente empêche le reboot/l'OTA demandé ; l'appelant reçoit une erreur. Sans compteur actif, aucune écriture n'est nécessaire.
- Une écriture déjà en cours reste propriétaire de son reçu même si l'appelant cesse d'attendre. Une demande de remise à zéro qui n'a pas encore été commencée expire au bout du délai ; elle ne s'exécute pas tardivement.
- Les mises à jour OTA habituelles conservent la NVS. Au démarrage, le dernier compte sauvegardé est restauré. Les impulsions reçues après le snapshot final ou pendant le redémarrage ne sont pas garanties.
- Les coupures d'alimentation et redémarrages d'urgence restent soumis au dernier checkpoint réussi : jusqu'à environ une heure de perte en fonctionnement normal, davantage en cas d'échec de sauvegarde. Le watchdog conserve son redémarrage de secours si la commande de reboot contrôlé échoue.
- Un effacement de la NVS ou de toute la flash supprime la sauvegarde. Si `pulse_v1` est absent, le cumul repart à zéro : aucune migration des anciens totaux flottants n'est effectuée. Un blob présent mais invalide est signalé comme une erreur, sans effacement silencieux.

### Remise à zéro depuis la Configuration

Le champ technique `counter_reset` est représenté par un bouton traduit « Remettre le compteur à zéro », visible uniquement en mode compteur. Les métadonnées de configuration déclarent un widget `action`, son endpoint, son identifiant numérique d'entrée et ses textes ; le frontend utilise un rendu générique.

La confirmation identifie l'entrée concernée et avertit de l'effacement du cumul. L'action n'est pas incluse dans l'enregistrement ordinaire de configuration. Le bouton est désactivé pendant l'opération et le POST n'est jamais répété automatiquement. Le succès n'est affiché qu'après confirmation de l'écriture NVS ; en cas de réponse incertaine, l'interface invite à vérifier le compteur avant de réessayer.

`POST /api/io/counter/reset` (`id` numérique, formulaire URL-encoded) appelle la commande `io.counter.reset` (`{"id":64}` pour I00). Le firmware valide l'identifiant et le mode effectif, puis exécute la remise à zéro dans la tâche IO. Le cumul nul et sa génération sont sauvegardés ensemble ; les autres entrées sont conservées. Les impulsions suivantes continuent à être comptées. Le jeton de configuration historique reste compatible mais n'est pas modifié par cette commande.

Les tests `scripts/tests/test_pulse_persistence.py` et `scripts/tests/test_counter_reset_action.cjs` couvrent la persistance, les erreurs/délais/concurrences, la restauration, la confirmation UI et l'absence de répétition automatique.

## DataStore

Le module publie ses valeurs runtime via `Modules/IOModule/IORuntime.h`.

Écritures utilisées:

- `setIoEndpointFloat(...)`
- `setIoEndpointBool(...)`
- `setIoEndpointInt(...)`

Clés utilisées:

- base `DataKeys::IoBase`
- un index runtime par endpoint exposé

## MQTT runtime

`IOModule` implémente `IRuntimeSnapshotProvider`.

Routes publiées actuellement:

- `rt/io/input/aN`
- `rt/io/input/iN`
- `rt/io/output/dN`

Le module peut aussi construire des snapshots de groupe:

- entrées
- sorties

## Runtime UI

Le module expose actuellement des valeurs Runtime UI pour:

- température eau
- température air
- pH
- ORP
- compteur eau

Les identifiants sont déclarés dans `IOModule::RuntimeUiValueId`.

## Home Assistant

Le module IO n'enregistre pas seul toutes les entités Home Assistant. Dans le profil Waveshare, `src/Profiles/Waveshare/WaveshareIoAssembly.cpp` synchronise:

- les capteurs analogiques déclarés
- les binary sensors ou sensors des entrées digitales
- les switches liés aux sorties digitales présentes

Cette synchronisation repose sur:

- `HAModule`
- la table `PoolBinding`
- les suffixes runtime `rt/io/...`

## Particularités de fonctionnement

- les sorties peuvent être impulsionnelles (`momentary`)
- les PCF8574 déclarés dans `expanderXX` sont réservés aux endpoints IO génériques
- les compteurs digitaux peuvent être persistés en NVS
- le module maintient `IoCycleInfo` pour exposer la liste des `IoId` modifiés sur le dernier cycle
- les labels exposés par `endpointLabel()` viennent des définitions construites par le profil

## Commande des équipements

Le module expose également la réservation des sorties, `writeAnalog`, les endpoints analogiques `o00..o03` déclarés par le profil et une tâche RS485 dédiée à un tick FreeRTOS. La boucle d'acquisition IO reste séparée. Le service de registres accepte un profil série par transaction et le format explicite Vendor Register RTU en plus du Modbus standard.

Voir [Pilotage des équipements](PoolActuators.md) pour les descripteurs, les fonctions constructeur, la distinction commande/observation et les limites matérielles.

### Valeurs calculées et Home Assistant

Les 16 emplacements `io/value/v00` à `io/value/v15` disposent d'un booléen
persistant `enabled`. Il vaut `false` par défaut pour tous les emplacements.
Une activation déjà enregistrée en NVS est conservée. Une source à `65535` (`VALUE_INVALID`) laisse l'emplacement inactif,
quel que soit `enabled`. Les changements de définition prennent effet après
redémarrage.

Une valeur désactivée n'est pas enregistrée dans le registre et ne dispose plus
de route MQTT ni de nouvelles données d'historique. Ses valeurs dépendantes
doivent être désactivées ou recevoir une autre source : une dépendance absente
est une erreur de configuration IO, comme un cycle ou une transformation invalide.

Lorsque MQTT et Home Assistant sont activés, chaque valeur effectivement publiée
crée un capteur Discovery `io_value_vNN`, nommé `Value VNN`, lié à
`<prefix>/rt/value/<96 + NN>`. Le capteur lit `value` et devient indisponible si
`quality` n'est pas `1` (valide), ou si Flow.io est hors ligne. Aucune unité n'est
supposée pour ces transformations génériques. Au démarrage, les entrées Discovery
des emplacements désactivés ou sans source sont retirées par un message retained
vide, afin de supprimer les anciens capteurs dans Home Assistant.

Les erreurs d'initialisation Discovery du profil Waveshare (allocation, préparation
ou enregistrement d'entités) sont journalisées avec `HA discovery failed` et ne
bloquent pas le démarrage. Les entités indépendantes continuent d'être enregistrées
lorsque leurs ressources sont disponibles. Une préparation des rôles en échec
empêche uniquement l'enregistrement des entités qui utilisent ces rôles. La
Discovery étant réalisée une fois au démarrage, un nouvel essai d'enregistrement
nécessite un redémarrage après correction de la cause. Ce comportement ne change
pas la validation des dépendances des valeurs par le module IO.

Le champ facultatif `io/value/vNN/name` définit le nom affiché dans Home Assistant
(63 octets UTF-8 maximum). Vide par défaut, il conserve le libellé `Value VNN`.
Le nom prend effet après redémarrage ; le topic Discovery et l’identifiant unique
restent identiques lors du renommage, y compris pour les capteurs déjà créés.
