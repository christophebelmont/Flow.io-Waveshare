# Préparation : prise en charge Polytropic plus complète

## Périmètre analysé

L'intégration [hass-integration-polytropic-heatpump](https://github.com/verwilst/hass-integration-polytropic-heatpump)
décrit une PAC IVS/IVN testée sur une Blueplus 17 kW Full Inverter, reliée à une
passerelle RS485/TCP. Elle annonce des lectures Modbus `0x03`, des écritures
`0x06`, une adresse par défaut 17 et un ensemble de registres allant de 57 à
1001. Flow.io prend déjà en charge les commandes de base, la vérification des
registres 1000/1001, la limite 58 et une télémétrie partielle ; ce changement
étend la lecture aux diagnostics complémentaires.

Ce dépôt HA est une source d'observations d'implémentation, pas une spécification
constructeur. Ses descriptions d'échelle, de bit et de registre doivent être
recoupées avec la table Polytropic correspondant au modèle et au firmware avant
d'être traitées comme garanties.

## Carte des registres et masques complémentaires

Toutes les lectures proposées emploient `0x03`, et les écritures de commande
`0x06`. Les registres 60 et 61 sont explicitement exclus par le dépôt HA : ne
pas les inclure dans une lecture groupée.

| Registre(s) | Données rapportées | Flow.io aujourd'hui |
|---|---|---|
| 57 | Compensation, signé, dixièmes de degré | Absent |
| 58 | Température d'eau cible maximale, dixièmes de degré | Lu avant écriture comme limite |
| 59 | Mode de pompe de circulation (0 toujours, 1 suit compresseur) | Absent |
| 62 | Modes de fonctionnement pris en charge (0–3) | Absent |
| 500 | États de composants sous forme de bits | Partiellement décodé |
| 501 | Défauts/protections, dont FLO au bit 14 | Bit FLO exploité pour la protection de marche |
| 502 | Défauts électriques/onduleur | Absent |
| 503 | Communication PCB, EEPROM, dégivrage | Bit dégivrage actuellement lu |
| 504–505 | Ouvertures des détendeurs électroniques 1/2 (0–500 pas) | Absentes |
| 506–507 | Vitesses ventilateurs 1/2, annoncées 0–999 RPM | Absentes ; échelle à confirmer |
| 508–509 | Niveaux ventilateurs 1/2 (0–3) | Absents |
| 510–516 | Températures compresseur, eau, échangeur, ambiance, IPM ; signé /10 | Présent, noms fonctionnels différents pour compresseur |
| 517–518 | Fréquences cible et actuelle (Hz, 0–120) | Absentes |
| 519–520 | Durées de fonctionnement et d'arrêt du compresseur (minutes) | Absentes |
| 521 | Tension AC (V) | Présente |
| 522 | Courant AC en dixièmes d'ampère | Présent, échelle /10 |
| 523 | Code défaut compresseur | Présent |
| 1000 | Mot de commande : bits 0–3 mode, bit 4 marche | Lu comme retour critique et télémétrie brute |
| 1001 | Consigne eau en dixièmes de degré | Lu comme retour critique et télémétrie |

Le dépôt HA regroupe 500–523 en une lecture de 24 registres. Ce bloc est
contigu et reste sous la limite de 32 registres du scheduler Flow.io. Il fait
évoluer le balayage actuel en cinq petits blocs (500–501, 503, 510–516,
521–523, 1000–1001) et récupère les registres encore manquants sans lire
d'adresse intermédiaire inconnue. Les registres 57–59 forment un bloc de trois
et 62 doit rester isolé.

Les masques ci-dessous sont fournis comme données complémentaires par le
demandeur. Ils permettent de définir des états nommés sans reprendre les noms
et masques non vérifiés du dépôt HA. Ils restent à valider sur la PAC ciblée.

| Registre | Masque | État / code signalé |
|---|---:|---|
| 500 | `0x0001`, `0x0002`, `0x0008` | Pressostats haute/basse pression, détecteur de débit |
| 500 | `0x0020`, `0x0040`, `0x0080`, `0x0100`, `0x0200` | Pompe, résistance électrique, vanne 4 voies, plaque et carter compresseur chauffants |
| 501 | `0x0001`–`0x8000` | Défauts de sondes AL01–AL06, protections AL10–AL17, débit FLO et antigel, selon le mapping fourni |
| 502 | `0x0001`–`0x0100`, `0x0400` | Défauts AL09, AL18–AL25 et communication driver EA07, selon le mapping fourni |
| 503 | `0x0001`, `0x0002`, `0x8000` | Communication PCB AL07, EEPROM AL08, dégivrage |

Les masques 501 et 502 couvrent presque un mot entier. Pour éviter de multiplier
les identités de mesures, Flow.io devrait publier les mots bruts `alarm_501`,
`alarm_502` et `alarm_503`, et éventuellement un indicateur calculé
`alarm_active`. Le décodage nommé peut être fourni dans le détail diagnostique
Web/HA, sans créer un identifiant métrique par bit. Les états de composants de
500 déjà exposés peuvent garder leurs identités stables.

## Commandes et trames

Le format de trame suit Modbus RTU standard : adresse esclave, fonction, données
big-endian et CRC Modbus transmis octet faible puis fort. L'intégration HA
n'embarque pas de trace brute prouvant le comportement électrique ou la polarité
des bits ; elle décrit des appels de bibliothèque. Les échanges existants de
Flow.io et la table externe de passerelle restent les références pour le codec.

Le mot 1000 combine mode (nibble bas) et marche (bit 4). Le profil Flow.io
compose déjà les valeurs de modes de la gamme (`0x11` à `0x17`) et commande ON
avec le bit 4. Le dépôt HA propose aussi les modes HVAC Auto/Heat/Cool et des
presets Normal/Boost/Silent. Ces noms HA ne doivent pas être recopiés tels quels
dans l'API Flow.io : les valeurs visibles dépendent de la correspondance
`MODE_ID_TO_HVAC_PRESET` du dépôt, à comparer à la table Polytropic avant
d'ajouter des sélections utilisateur.

Les opérations HA lisent la configuration via `0x03` et écrivent les paramètres
avec `0x06`. Dans Flow.io, l'écriture de consigne avant marche, l'arrêt, la
validation de la limite du registre 58, le read-back et la confirmation
asynchrone existent déjà. Les mesures de 1000/1001 restent le retour critique ;
les nouveaux diagnostics ne doivent pas se substituer à cette preuve.

## Périmètre implémenté

La télémétrie Flow.io lit désormais deux blocs à basse priorité : `500..523`
et `1000..1001`. Elle expose aussi les détendeurs (504–505), les vitesses et
niveaux de ventilateur (506–509), les fréquences cible/actuelle (517–518) et
les durées compresseur actif/à l'arrêt (519–520). Les temps d'arrêt aident à
diagnostiquer un démarrage attendu qui tarde.

Les mots d'alarme 501–503 restent disponibles en valeurs brutes. Les masques
fournis sont également décodés en indicateurs lisibles dans le détail Web/HA,
sans créer une identité de valeur/formule par bit. Les mesures indépendantes
restent réservées aux valeurs numériques et états de composants utiles au
diagnostic et aux formules.

Les registres 57/59 et capacité 62 ne sont pas interrogés en continu. Aucune
puissance active n'est calculée à partir de V×A sans facteur de puissance et
nombre de phases.

Les valeurs de vitesse ventilateur sont signalées ici comme non documentées sur
la machine concernée, malgré la plage 0–999 RPM annoncée dans le dépôt HA. Les
publier initialement comme valeurs brutes ou avec unité explicitement provisoire,
et les valider par comparaison à l'afficheur/service constructeur. Même réserve
pour les registres de temps : confirmer qu'ils sont en minutes et établir leur
comportement lors d'un redémarrage ou d'un retour à zéro.

La lecture groupée suppose que toute la plage 500–523 est lisible sur les
modèles ciblés. Elle évite de multiplier les transactions RS485 et ne lit pas les
registres 60–61.

## Travail d'implémentation proposé

1. **Décoder les mesures sans logique par position implicite** : définir pour
   chaque champ son registre, son type (bit, entier, signé), son échelle, son
   unité et sa fraîcheur. Appliquer les masques 500–503 fournis dans une table
   explicite, en distinguant code défaut de booléen d'alarme.
2. **Publier le catalogue** : les nouvelles mesures sont dans
   `src/Core/Values/equipment_metrics.json`, puis régénérées avec le catalogue
   Web/HA. Le stride a été étendu à 40 sans changer les identifiants historiques :
   les métriques 0–31 conservent leur plage par équipement et les nouvelles
   métriques 32–39 utilisent la plage additionnelle allouée après les 16 plages
   historiques. Toute évolution future doit préserver cette répartition.
3. **Compléter les libellés** : les valeurs numériques et booléennes sont
   traduites en français et en anglais dans le détail Web/HA.
4. **Protéger l'ordonnanceur** : les blocs de diagnostic restent à basse priorité,
   préserver l'alternance avec les confirmations de commande, et recalculer les
   délais minimums `stale_ms` / `telemetry_stale_ms` avec le nouveau nombre de
   blocs, les retries et le trafic partagé. Une mesure invalide ou périmée doit
   rester indisponible, sans remettre en cause seule la marche.
5. **Séparer les niveaux de certitude** : les champs 57/59/62 et les bits
   d'alarme sont des affirmations du projet HA jusqu'à validation sur table
   Polytropic et sur l'équipement ciblé. Ne pas tirer de puissance électrique
   de V×A sans connaître facteur de puissance et phases ; le dépôt HA calcule
   `input_power = voltage × current`, mais cette approximation n'est pas une
   puissance mesurée.

## Points à trancher par les sources constructeur / banc

- La table constructeur confirmant 57, 59, 62, 502 et 504–520 ainsi que leurs
  droits de lecture, types, unités et éventuels codes sentinelles.
- La définition exacte de chaque bit des registres 500–503 : les masques
  complémentaires sont plus détaillés que le dépôt HA, mais restent à valider
  avec la documentation constructeur ou sur l'équipement.
- La polarité du bit 4 ON/OFF et les valeurs de mode pour la variante réellement
  raccordée ; la polarité est déjà signalée comme à vérifier dans la
  documentation Flow.io.
- L'échelle réelle des vitesses ventilateur, les unités et le comportement de
  rollover/réinitialisation des durées 519–520.
- Les échelles de tension/courant, et si `×V` `×A` donne une puissance active.
- Le comportement de lecture groupée sur 500–523 pour les versions ciblées.

## Références

- [README de l'intégration](https://github.com/verwilst/hass-integration-polytropic-heatpump) : matériel testé, entités et réglages.
- [Carte de registres `const.py`](https://github.com/verwilst/hass-integration-polytropic-heatpump/blob/main/custom_components/polytropic_heatpump/const.py) : commentaires d'adresses et d'échelles.
- [Coordinateur `coordinator.py`](https://github.com/verwilst/hass-integration-polytropic-heatpump/blob/main/custom_components/polytropic_heatpump/coordinator.py) : séquences de lecture, décodage signé, mesures dérivées et écritures.
- [Commandes `climate.py`](https://github.com/verwilst/hass-integration-polytropic-heatpump/blob/main/custom_components/polytropic_heatpump/climate.py) : correspondances mode HVAC/presets et appels de commande.
- `docs/modules/PoolActuators.md` : profil et limites Polytropic existants dans Flow.io.
