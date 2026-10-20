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

`driver` est un document JSON stocké comme chaîne (`2048` octets maximum, terminateur inclus). Le formulaire Web présente ses champs. Une mise à jour par l'API de configuration doit donc encoder ce document comme chaîne :

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

### Modèles RS485 intégrés

Le formulaire expose uniquement **Modèle d’équipement** pour le protocole :
**AquaGem** ou **Polytropic IVS/IVN/IVP/IVR (>2018)**. Les dépendances métier restent indépendantes.
La configuration persistée est `{"kind":3,"serial":{"model":1}}` pour AquaGem,
`{"kind":3,"serial":{"model":2}}` pour Polytropic. Le firmware résout les paramètres
avec `PoolSerialProfiles.h`. Les surcharges de paramètres série d'un modèle sont refusées.
Un modèle absent de l'ancien format n'est jamais déduit de l'adresse ou d'un libellé :
les anciennes configurations restent lisibles au démarrage, mais leur édition Web exige
un choix explicite. Sélectionner un modèle remplace l'ancien paramétrage série.
La sélection s'applique après sauvegarde et redémarrage. Les anciens exemples candidats
ont été remplacés par ces configurations compactes.

**AquaGem**, d'après `RS485 Protocol.pdf`, tables 8–11 :

- Adresse `0xAA`, 1200 bauds, 8N1 ; vitesse 1200–2900 RPM, démarrage 2400 RPM.
- Lecture `C3`, adresse 2001 ; écriture `D0`, adresse 3001. Arrêt : valeur **1**.
- Réponse lecture : adresse, fonction, défauts 16 bits, état 8 bits, RPM 16 bits, CRC.
  Le bit 0 de l'état confirme la marche, indépendamment de la vitesse. Le masque de défauts
  est exposé par `device_faults` dans le snapshot équipement.
- Acquittement d'écriture : adresse, fonction, adresse de registre, trois octets réservés, CRC.
  Il ne confirme pas physiquement la rotation ; la lecture suivante fournit cette confirmation.
- CRC Modbus calculé sur le contenu, transmis **octet fort puis faible**, conformément à la note du PDF.
- Deux requêtes du pilote sont espacées d'au moins 500 ms ; silence de 50 ms après réponse.
  Les réessais du transport respectent une garde de 500 ms après échec.

La valeur des deux octets réservés de la requête de lecture n'est pas spécifiée :
le codec émet `00 00`. Cette convention et l'ordre du CRC doivent être vérifiés sur banc.
Aucune détection permissive d'ordre du CRC ou de variante de réponse n'est effectuée.
Aucun pas de 50 RPM n'est imposé, car il ne figure pas dans cette documentation.
Exemples de requêtes complètes : lecture `AA C3 07 D1 00 00 4D 0D`,
1700 RPM `AA D0 0B B9 06 A4 19 08`, arrêt `AA D0 0B B9 00 01 C2 CB`.

**Polytropic**, d'après `Modbus table for IV... (for external Gateway) v6.pdf` :

- Adresse par défaut 17, 9600 bauds, 8N1, Modbus standard `03`/`06`.
  Le profil intégré suppose l'adresse d'usine (le constructeur permet de la modifier).
- Registre 1000 : bits 0–3 pour le mode, bit 4 pour ON/OFF, bits 5–15 à zéro.
  Marche retenue : bit 4 à 1 ; arrêt : mot à zéro. La polarité du bit 4 reste à vérifier
  sur banc, le tableau indiquant ON/OFF sans détailler les deux valeurs.
- Modes marche : Auto `0x11`, Cooling Smart/Boost/Eco `0x12`/`0x13`/`0x14`,
  Heating Smart/Boost/Eco `0x15`/`0x16`/`0x17`. Mode initial : Heating Smart.
- Consigne 1001 en dixièmes de °C ; domaine documentaire 0–60 °C, démarrage 25 °C.
  Avant chaque nouvelle consigne de marche, lecture du maximum réel au registre 58
  (25–60 °C). Une consigne supérieure est refusée sans écriture ; l'arrêt reste possible.
- Retour critique : registre 1000 pour l'état ON/OFF et 1001 pour la consigne relue.
  Cela confirme l'état déclaré par la PAC, pas la rotation physique du compresseur.
- Télémétrie des températures, états, tension en V et courant en A.

Pour les machines produites à partir de 2022, le constructeur exige de déconnecter
la passerelle Polyconnect, avec perte des services associés. Seules les adresses
publiées sont utilisées. Aucun scan de registres n'est effectué.

### Ancien format générique Modbus RTU

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
- `unit` : `0` vitesse %, `1` puissance %, `2` °C, `3` RPM. La fonction de l'appareil doit effectivement accepter cette grandeur.
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

### Réponses constructeur avec écho du registre

Chaque opération peut définir `response` indépendamment de sa requête `layout` :

- `response=0` : format historique du tableau ci-dessus (valeur par défaut).
- `response=1` : `esclave | fonction | registre MSB | registre LSB | nombre d'octets | valeurs big-endian | CRC bas | CRC haut`.

Le second format est autorisé uniquement avec `protocol=1`, en lecture ou écriture simple. Pour un registre, la réponse compte exactement neuf octets et la valeur occupe les indices 5 et 6. Le codec vérifie adresse esclave, fonction, registre retourné, compteur, longueur exacte et CRC ; un acquittement d'écriture doit aussi restituer la valeur envoyée. Il n'y a ni détection automatique de variante ni acceptation d'un en-tête inconnu. Les exceptions Modbus restent réservées à `protocol=0`.

### Commande unifiée, quantification et retour numérique

| Champ série | Valeurs et comportement |
|---|---|
| `control` | `0` : consigne puis marche séparée ; `1` : écrire la consigne de marche ou `stop_value` directement dans l'opération `setpoint`. L'opération `run` est inutilisée dans ce dernier cas. |
| `raw_step` | Pas entier positif du registre, `1` par défaut. Les valeurs de marche sont des multiples de ce pas dans les limites configurées. |
| `raw_rounding` | `0` : au plus proche, demi-pas vers le haut ; `1` : vers le bas. L'arrondi reste borné aux pas admissibles entre minimum et maximum. |
| `running_source` | `0` : masque `running_mask` lu par `status` ; `1` : retour décodé strictement supérieur à `running_threshold`. Dans ce dernier cas, une seule lecture `feedback` fournit vitesse et marche. |
| `feedback_type` | `0` : entier non signé ; `1` : entier signé sur 16 bits en complément à deux, avant gain et offset. |

`stop_value` est envoyé tel quel, sans conversion ni quantification. En commande unifiée, il ne peut pas être une valeur de marche admissible. Un arrêt reste prioritaire. Sans retour, le renouvellement écrit de nouveau la vitesse courante, jamais une valeur fixe de marche. `applied.setpoint` et les estimations reflètent la valeur quantifiée, tandis que `setpoint` conserve la demande utilisateur.

### Modes de fonctionnement et PAC

`run_modes` est une liste facultative d'au plus huit objets `{"label":"Heating Eco","value":120}`. Les libellés (23 octets maximum) et valeurs doivent être uniques ; aucune valeur ne peut être le code d'arrêt. Cette liste est réservée à `control=0`. Le mode initial est l'index `0` ; sans liste, `run_value` reste utilisé. La consigne thermique est écrite avant la commande de marche/mode.

`pooldevice.mode`, args `{"slot":7,"value":2}`, sélectionne l'index du mode sans démarrer un appareil arrêté. `pooldevice.write` accepte aussi `mode` et `setpoint` pour une commande atomique. Les snapshots exposent les libellés dans `modes`, la demande dans `mode` et le mode acquitté dans `applied.mode`. Ce dernier n'est pas une confirmation physique du mode actif.

Le Web propose une liste de modes ; Home Assistant reçoit un `select`. Les unités RPM sont affichées sur le Web et dans le `number` Home Assistant. Pour le rôle chauffage en automatique, `PoolLogic` transmet sa consigne à un équipement déclaré en °C, en conservant le mode sélectionné. Un changement de température en cours de chauffe est transmis sans attendre un arrêt/redémarrage, à cadence bornée. Les relais et les autres unités conservent leur comportement.

`telemetry_profile=1` active la carte de registres PAC Poly du prototype, exclusivement en Modbus standard. Cinq blocs sont lus à basse priorité : `500`, `503`, `510..516`, `521..523`, `1000..1001`. La lecture groupée utilise `0x03`. Les températures sont signées, en dixièmes de degré. Les indicateurs du registre 500 et le dégivrage sont exposés comme booléens. Tension et courant sont exprimés en V et A selon la table constructeur v6. Le registre 1000 est également publié comme `control_word` brut.

La télémétrie apparaît dans `rt/pdm/state/pdN.telemetry`, dans le détail Web et dans les attributs du capteur diagnostic Home Assistant. Une mesure absente ou périmée vaut `null`. `telemetry_error` indique le résultat du dernier échange de diagnostic ; les valeurs non disponibles restent `null` même après la réussite d'un autre bloc. Une erreur de diagnostic ne confirme ni n'invalide à elle seule la marche : celle-ci garde son propre retour critique.

Les lectures de diagnostic alternent avec le retour critique. Avec ce profil, `stale_ms` doit couvrir au moins `4 * poll_ms + timeout_ms` et `telemetry_stale_ms` au moins `10 * poll_ms + timeout_ms` (30 secondes par défaut). Ces minima doivent encore être augmentés selon la charge du bus et les réessais. Le gel des écritures gèle également les nouveaux échanges série et laisse les mesures expirer.

### Exemples des profils intégrés

- [AquaGem](../examples/rs485/aquagem-candidate.json)
- [Polytropic](../examples/rs485/heatpump-poly-candidate.json)

La commutation 1200/9600 bauds sur la même paire exige une validation avec les deux
équipements raccordés. Ces fichiers ne modifient pas automatiquement la configuration NVS.

Les estimations de débit doivent passer par la courbe calibrée `flow_curve` en **L/h** (le prototype emploie des L/min). Aucune puissance électrique mesurée ni régulation hydraulique réelle n'est déduite des RPM. La boucle bloquante de cinq secondes et la formule de puissance non calibrée du prototype ne sont pas reprises.

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
