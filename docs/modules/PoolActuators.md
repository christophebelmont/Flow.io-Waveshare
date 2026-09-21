# Pilotage des équipements : relais, vitesses, analogique et RS485

Un `PoolDevice` conserve son rôle métier, ses dépendances et ses compteurs indépendamment du raccordement. `PoolDeviceDefinition::control` décrit son pilote par défaut. Le paramètre `driver` de `pdm/pdN` remplace ce descripteur au démarrage.

## Architecture

- `PoolDeviceModule` : propriétaire des consignes, interlocks, limites journalières et compteurs.
- `IPoolDeviceDriver` : `begin`, `applyTarget`, `tick`, `readState`. Aucun pilote n'attend une réponse série dans la tâche métier.
- `DigitalRelayDriver` : sortie booléenne.
- `DiscreteSpeedDriver` : jusqu'à six sorties exclusives, désactivation avant activation, temps mort non bloquant.
- `AnalogSetpointDriver` : conversion affine de la consigne vers une sortie analogique physique.
- `SerialDeviceDriver` : écriture de la consigne avant marche, arrêt prioritaire, télémétrie facultative.
- `IOModule` : endpoints, réservation des sorties et transport UART.
- `Rs485TransactionScheduler` : arbitre commun, une transaction active par ligne. Le profil Waveshare expose actuellement le bus `0`.

L'ancien module indépendant `VariableSpeedPumpModule` et son service ont été supprimés. Les consommateurs utilisent `PoolDeviceService::setRunning`, `setTarget` et `readState`.

## Configuration persistante

Clés NVS : `actN_driver`, `actN_en`, `actN_dp`, `actN_flh`, `actN_tc`, `actN_ti`, `actN_mu`, `actN_rt`. Aucune lecture, conversion ou migration des anciennes clés `pdN*`.

`driver` est un document JSON stocké comme chaîne (`1536` octets maximum, terminateur inclus). Le formulaire Web présente ses champs. Une mise à jour par l'API de configuration doit donc encoder ce document comme chaîne :

```json
{"pdm/pd0":{"driver":"{\"kind\":0,\"outputs\":[0]}"}}
```

Le validateur refuse les paramètres de pilote incohérents avant application du patch. Les ressources physiques et adresses en conflit sont vérifiées au démarrage ; un équipement invalide est bloqué et publie `driver_error`. Le raccordement et le protocole prennent effet après redémarrage. Une activation d'un équipement qui était désactivé au démarrage nécessite également un redémarrage pour réserver ses ressources.

Les champs métier `enabled`, `depends_on_mask`, `flow_l_h`, `tank_cap_ml`, `tank_init_ml`, `max_uptime_day_s` restent disponibles. Le masque de dépendances est sur 16 bits ; le profil expose huit équipements. Les cycles et indices hors capacité ne satisfont jamais les interlocks.

### Relais

```json
{"kind":0,"outputs":[0]}
```

Les éléments de `outputs` sont des `IoId`, pas des numéros de GPIO. `0..15` désignent les sorties digitales du profil. Les sorties impulsionnelles ne sont pas utilisables par les pilotes d'équipement.

### Trois vitesses par sorties digitales

```json
{
  "kind":1,
  "outputs":[8,9,10],
  "steps":[30,60,100],
  "minimum":30,
  "maximum":100,
  "startup":60,
  "dead_ms":250
}
```

Les vitesses sont strictement croissantes. Une consigne doit correspondre exactement à un palier ; pas d'arrondi implicite. À l'arrêt toutes les sorties sont désactivées. Un échec de désactivation interdit l'activation suivante. Une nouvelle consigne pendant le temps mort remplace la précédente.

### Appareil Modbus RTU

Exemple de format, **adresses de registres illustratives à remplacer par celles du constructeur** :

```json
{
  "kind":3,
  "unit":0,
  "minimum":0,
  "maximum":100,
  "startup":60,
  "serial":{
    "bus":0,
    "address":1,
    "baud":9600,
    "parity":0,
    "stop_bits":1,
    "protocol":0,
    "run":{"address":0,"function":6},
    "setpoint":{"address":1,"function":6},
    "has_feedback":true,
    "status":{"address":2,"function":3},
    "feedback":{"address":3,"function":3},
    "run_value":1,
    "stop_value":0,
    "running_mask":1,
    "raw_per_unit":10,
    "feedback_gain":0.1,
    "timeout_ms":300,
    "poll_ms":1000,
    "stale_ms":5000,
    "retries":1,
    "quiet_ms":5,
    "late_guard_ms":100
  }
}
```

- `kind` : `0` relais, `1` paliers, `2` analogique, `3` RS485.
- `unit` : `0` vitesse %, `1` puissance %, `2` °C. La fonction de l'appareil doit effectivement accepter cette grandeur.
- `parity` : `0` aucune, `1` paire, `2` impaire ; stop bits `1` ou `2`.
- `protocol=0` : Modbus RTU standard, lecture `3/4`, écriture `6/16`.
- Conversion écriture : `raw = round(setpoint * raw_per_unit + raw_offset)`, registre non signé 16 bits. Les limites doivent tenir dans `0..65535`.
- Conversion lecture : `value = raw * feedback_gain + feedback_offset`.
- `poll_ms` est l'intervalle entre deux lectures alternées état/consigne, pas la période de l'ensemble des mesures.
- `stale_ms` doit couvrir au minimum `2 * poll_ms + timeout_ms` et être dimensionné pour la charge du bus.
- Sans retour, les écritures marche/arrêt **idempotentes** sont renouvelées à `poll_ms`. L'état reste estimé ; l'acquittement ne prouve pas la rotation.

### Codes constructeur `0xC3` / `0xD0`

`protocol=1` sélectionne explicitement **Vendor Register RTU**. Ce format conserve adresse esclave, registres big-endian et CRC Modbus. Il n'applique pas le bit d'exception Modbus aux codes constructeur.

Chaque opération définit `function` (octet en décimal dans JSON) et `layout` :

| Layout | Requête | Réponse attendue |
|---|---|---|
| `0` | registre + nombre de registres | nombre d'octets + valeurs |
| `1` | registre + valeur | écho registre + valeur |
| `2` | registre + nombre + nombre d'octets + valeurs | écho registre + nombre |

Par exemple : `"status":{"address":2,"function":195,"layout":0}` et `"setpoint":{"address":1,"function":208,"layout":1}`.

Un protocole ayant un autre checksum, préambule, adressage, encodage numérique ou format d'acquittement nécessite un codec adapté. Le logiciel ne déduit pas ces propriétés du seul code de fonction et ne prétend pas prendre en charge un constructeur non documenté.

### Sortie analogique

```json
{"kind":2,"outputs":[256],"minimum":0,"maximum":100,"startup":60,"gain":0.1,"offset":0,"off":0}
```

`o00..o03` (`IoId` `256..259`) sont disponibles pour les endpoints analogiques déclarés par le profil matériel avec `IOModule::defineAnalogOutput`. Ils utilisent `AnalogActuatorEndpoint`, la même registry et les snapshots `rt/io/output/oNN`. Le profil fournit une fonction d'écriture, sa plage physique et sa durée de vie.

`PwmAnalogOutput` fournit un backend PWM Arduino LEDC configurable (fréquence, résolution, maximum, valeur de démarrage). Le profil doit lui attribuer un GPIO libre ; les sorties d'expander/relais ne produisent pas de PWM. Le câblage 0–10 V / 4–20 mA, le filtrage et la conversion électrique restent externes. Aucun GPIO supplémentaire ni DAC fictif n'est activé par défaut sur la carte Waveshare.

## État, commande et sécurité

Le runtime expose `desired`, `setpoint`, `effective`, `applied`, `observed`, `quality`, `phase`, `online`, `error` et les révisions acquittées. Un `OK` de commande signifie **accepté**.

- Qualité : `0` inconnu, `1` estimé, `2` confirmé par lecture, `3` périmé.
- Phase : `0` initial, `1` en attente, `2` appliqué, `3` échec, `4` écritures gelées.
- La consigne de marche est indépendante de la valeur : zéro ne constitue pas une commande universelle d'arrêt.
- Une erreur ou un retour périmé retire l'ancienne demande de marche ; pas de reprise automatique de cette demande.
- `readActualOn` renvoie `NOT_READY` quand l'observation n'est pas exploitable. La lecture de sortie locale est estimée, pas un capteur de rotation.
- Une sortie locale indisponible invalide immédiatement l'observation, même lorsque les écritures sont gelées. Une incohérence entre les broches d'un pilote à paliers impose une nouvelle séquence de désactivation et de temps mort.
- `dependency_minimum` impose un niveau minimal aux dépendances ; `dependency_confirmed` impose un retour confirmé. Un pourcentage ne garantit pas un débit hydraulique.
- Les sorties sont réservées au démarrage, avec contrôle des alias physiques. Désactiver un équipement déjà propriétaire l'arrête sans libérer ses sorties à chaud.
- Le gel des écritures annule les opérations série en attente ; une trame déjà émise est drainée avant de libérer le bus.

Pour le dosage variable avec suivi de cuve, `flow_curve` est obligatoire et doit couvrir toute la plage : `[[0,0],[50,800],[100,2400]]` associe consigne et L/h. L'interpolation est linéaire entre points calibrés ; les volumes restent des estimations. Aucune assimilation automatique vitesse % / débit %.

## Ordonnancement RS485

File de 16 transactions, buffers de 256 octets, jusqu'à 32 registres par transaction. Priorité arrêt/sécurité puis commande puis lecture ; une demande ordinaire en attente depuis deux secondes remonte devant le trafic ordinaire plus récent. Une tentative échouée rend la main à l'arbitre avant réessai.

Seule la tâche `rs485` de `IOModule` utilise le transport. Elle tourne à un tick FreeRTOS, indépendante de l'acquisition IO. Le baud/parité/stop sont appliqués uniquement lorsque le bus est libre, après les silences et la garde contre les réponses tardives. L'annulation par un client ne tronque jamais une émission active.

Changer le baud de l'ESP32 ne garantit pas la coexistence électrique/protocolaire d'appareils différents sur la même paire : cette combinaison doit être vérifiée sur le banc réel. Des appareils bavards ou incompatibles nécessitent des segments distincts. Le firmware actuel ne déclare qu'un UART RS485 physique, `bus=0`.

## Commandes et interfaces

- `pooldevice.write`, args `{"slot":0,"value":true,"setpoint":60}` : marche et consigne atomiques ; `setpoint` facultatif.
- `pooldevice.setpoint`, args `{"slot":0,"value":65.5}` : modifie le niveau sans démarrer l'appareil.
- Les commandes métier `poollogic.*` conservent l'arbitrage manuel/automatique de PoolLogic.
- Web : sélection de palier ou saisie numérique, observation et qualité dans le dialogue équipements.
- Home Assistant : `select` pour les paliers, `number` pour les consignes continues ; les switches utilisent les équipements indépendamment de leur raccordement.
- MQTT : snapshots `rt/pdm/state/pdN` et `rt/pdm/metrics/pdN` ; configuration `cfg/pdm/pdN`.

## Vérification

```sh
python3 -m unittest discover -s scripts/tests -p 'test_pool_actuators.py'
python3 -m unittest discover -s scripts/tests -p 'test_generate_runtimeui_manifest.py'
node scripts/tests/test_pool_setpoint_dialog.cjs
node scripts/tests/test_device_dialog_live_updates.cjs
pio run -e Flowio-waveshare-esp32-s3
```

Les tests hôtes compilent les vrais pilotes/codecs/ordonnanceur avec de faux transports. Les tests navigateur nécessitent Playwright. Les temporisations UART, retours constructeur et niveaux électriques doivent encore être validés sur le matériel raccordé ; aucun équipement réel n'a été flashé ou commandé pendant cette implémentation.
