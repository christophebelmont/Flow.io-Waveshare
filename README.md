<p align="center">
  <img src="docs/pictures/Logo_flowio.png" alt="Logo flow.io" width="336">
</p>

<p align="center">
  <strong>Le pilotage intelligent et local des équipements de votre piscine.</strong>
</p>

flow.io surveille le bassin, automatise les traitements et coordonne les équipements de la piscine depuis un contrôleur unique. L'objectif est simple : conserver une eau stable, éviter les fonctionnements inutiles et rendre l'entretien quotidien plus prévisible.

## Exemple d'installation

<p align="center">
  <img src="docs/pictures/flowio-installation-marketing-v2.jpg" alt="Installation complète du système flow.io avec pompes péristaltiques, sondes et points d'injection" width="574"><br>
  <em>Exemple d'installation en cours du système flow.io.</em>
</p>

## Ce que flow.io fait pour votre piscine

### Maintenir une eau équilibrée

flow.io suit les principales mesures utiles au traitement de l'eau :

- température de l'eau et de l'air ;
- pH et potentiel redox (ORP) ;
- pression du circuit hydraulique ;
- niveau d'eau du bassin ;
- niveau des cuves de traitement ;
- consommation d'eau et temps de fonctionnement des équipements.

À partir de ces informations et des réglages choisis, flow.io adapte les actions de traitement tout en tenant compte des conditions de sécurité.

### Adapter automatiquement la filtration

La durée de filtration peut évoluer avec la température de l'eau et les horaires autorisés. flow.io coordonne également la filtration avec les traitements, le chauffage, le robot et les autres équipements qui en dépendent.

Les modes automatique, manuel, hivernage et protection contre le gel permettent d'adapter le fonctionnement à la saison et aux besoins ponctuels.

### Piloter le traitement de l'eau

flow.io prend en charge plusieurs stratégies de désinfection :

- dosage de chlore ou de brome à partir de la mesure ORP ;
- électrolyse au sel, selon une consigne ORP ou des plages de fonctionnement ;
- dosage planifié d'oxygène actif liquide selon le volume du bassin et les paramètres du produit.

La correction du pH peut être pilotée avec une pompe doseuse pH− ou pH+. Les injections sont conditionnées par l'état de la filtration, la disponibilité des mesures, la pression hydraulique et les limites de fonctionnement configurées.

### Coordonner les équipements

Selon l'installation, flow.io peut commander :

- la pompe de filtration ;
- les pompes doseuses pH et désinfection ;
- un électrolyseur au sel ;
- le chauffage ou la pompe à chaleur ;
- un robot de piscine ;
- le remplissage automatique ;
- l'éclairage et des relais auxiliaires.

Les dépendances entre équipements sont gérées de manière centralisée. Par exemple, un traitement ou un chauffage qui nécessite une circulation d'eau ne fonctionne pas indépendamment de la filtration.

## Vue d'ensemble

<p align="center">
  <img src="docs/pictures/flowio-pool-ecosystem.png" alt="Schéma de principe d'une installation de piscine pilotée par flow.io" width="900">
</p>

Le contrôleur reçoit les mesures du bassin et des cuves, puis commande les équipements de filtration, de traitement et de confort. Les informations restent consultables localement et peuvent également être transmises à une installation domotique.

## Une supervision locale et distante

flow.io propose plusieurs moyens de suivre et de piloter la piscine :

- une interface web adaptée à l'ordinateur et au téléphone ;
- un écran tactile local pour les mesures et commandes essentielles ;
- une intégration MQTT et Home Assistant ;
- des historiques et tableaux de bord possibles avec les outils compatibles MQTT ;
- des alarmes pour les états qui nécessitent une intervention ;
- des mises à jour du système par le réseau local.

### Piloter sa piscine avec Home Assistant et MQTT

L'interface Home Assistant permet de consulter les mesures et de piloter sa piscine depuis partout dans le monde, directement depuis un smartphone.

Grâce à MQTT, flow.io peut également être intégré à d'autres systèmes domotiques compatibles, notamment Jeedom, openHAB et Domoticz.

<p align="center">
  <img src="docs/pictures/flowio-home-assistant-mobile-marketing.png" alt="Interface Home Assistant de flow.io sur smartphone avec les logos Home Assistant, Jeedom, openHAB et Domoticz" width="1100"><br>
  <em>Pilotage de la piscine à distance avec Home Assistant et intégration aux principaux systèmes domotiques compatibles MQTT.</em>
</p>

### Une interface web claire et complète

L'interface web réunit le tableau de bord, l'état de la piscine, les mesures, les équipements, les alarmes, l'historique d'activité et les mises à jour. Elle propose des thèmes clair et sombre et reste accessible directement depuis le réseau local, sans dépendre d'un service en ligne.

<p align="center">
  <img src="docs/pictures/flowio-web-interface-marketing.png" alt="Interface web de flow.io présentée dans plusieurs fenêtres Safari" width="1100"><br>
  <em>Supervision et configuration de la piscine depuis l'interface web flow.io.</em>
</p>

### Un écran tactile local

L'écran tactile [Nextion](https://nextion.tech/) peut être installé dans le local technique via un câble série, ou déporté, grâce à sa connexion Wi-Fi. Plusieurs modèles sont supportés en définition 480x320 ou 800x480, en version Enhanced ou Intelligent et en capacitif ou résistif selon le budget et les fonctionnalités attendues.

<p align="center">
  <img src="docs/pictures/Nextion5-2-marketing.png" alt="Écran tactile Nextion flow.io installé près d'une piscine" width="900">
  <em>Ecran Nextion présenté dans un boitier dédié.</em>
</p>

## Le matériel flow.io

L'installation de référence associe un contrôleur industriel Waveshare ESP32-S3 à la carte flow.io Companion, dans un boîtier de contrôle complété par un écran tactile local.

<p align="center">
  <img src="docs/pictures/flowio-controller-enclosure-cutaway-middle.png" alt="Carte Flow.io Companion dans son boitier standard" width="1100"><br>
  <em>Carte flow.io Companion dans son boitier.</em>
</p>

<p align="center">
  <img src="docs/pictures/flowio-system-marketing-overview-v3.png" alt="Vue d'ensemble du système flow.io, de ses capteurs et des équipements de piscine pilotés" width="1100"><br>
  <em>Liaison entre le boîtier flow.io et le contrôleur Waveshare par une nappe standard.</em>
</p>

Le contrôleur [Waveshare ESP32-S3-POE-ETH-8DI-8RO](https://www.waveshare.com/product/iot-communication/esp32-s3-eth-8di-8ro.htm) assure le fonctionnement autonome et les communications Ethernet ou Wi-Fi.

La carte flow.io Companion regroupe les raccordements utiles à la piscine sur des borniers et connecteurs clairement identifiés : sondes pH et ORP, pression, températures, niveaux, compteur d'eau, afficheur et extensions. Elle simplifie le câblage et permet une intégration plus compacte et plus lisible dans le coffret technique.

Le boîtier rassemble l'électronique de contrôle dans un ensemble compact, tandis que l'écran tactile donne accès sur place aux mesures essentielles et aux commandes de la piscine.

## Au quotidien

Une fois l'installation configurée, flow.io travaille en continu pour :

- ajuster la filtration aux conditions du bassin ;
- maintenir les traitements dans les plages choisies ;
- empêcher certaines commandes lorsque les conditions de sécurité ne sont pas réunies ;
- signaler les niveaux bas, les défauts de pression ou les mesures indisponibles ;
- conserver les temps de marche et les volumes injectés ;
- donner une vue d'ensemble de la piscine, sur place ou à distance.

Le résultat recherché est une eau plus régulière, moins d'interventions répétitives et une meilleure maîtrise des consommations de produits et d'énergie.

---

Pour l'installation, la compilation, la cartographie des raccordements et la structure du logiciel, consulter la [documentation technique de flow.io](docs/README.md).
