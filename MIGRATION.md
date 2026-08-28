# Historique de la spécialisation Waveshare ESP32-S3

Ce document conserve l'historique de la transformation de l'ancien dépôt
flow.io multi-cibles en une arborescence dédiée à `Flowio-waveshare-esp32-s3`.
Il décrit l'état de l'export initial ; pour la documentation technique actuelle,
consulter [`docs/README.md`](docs/README.md).

## Résultat

- une seule section PlatformIO: `[env:Flowio-waveshare-esp32-s3]`;
- un seul profil applicatif et un seul catalogue matériel/domaine;
- l'export initial ne contenait ni métadonnée Git, ni cache PlatformIO, ni
  binaire généré;
- aucune valeur Wi-Fi ou MQTT sensible compilée par défaut;
- compilation de production validée le 25 août 2026.

La configuration matérielle de référence est centralisée dans
`src/Board/WaveshareBoard.h`. Elle regroupe notamment les UART, bus I2C et
OneWire, le W5500, le TFT, le buzzer, les GPIO et les capacités statiques.

## Phases réalisées

1. Inventaire des huit environnements PlatformIO, macros de sélection,
   profils, cartes, domaines, scripts, partitions et dépendances.
2. Création d'un état intermédiaire où la cible Waveshare était la seule cible
   de build, sans supprimer les abstractions.
3. Compilation réussie de cet état intermédiaire.
4. Suppression des profils, modules, domaines, cartes, simulations et branches
   conditionnelles qui n'étaient pas utilisés par Waveshare.
5. Consolidation du catalogue mono-carte autour de `WaveshareBoard.h` et
   remplacement des derniers noms de structures matérielles liés à
   l'architecture Supervisor par des noms locaux.
6. Compilations successives et correction des références, documents et
   commandes devenus obsolètes.
7. Nettoyage des sorties générées et préparation de la copie sans historique.

## Suppressions principales

- environnements `FlowIO`, `FlowIOWokwi`, `WaveshareWokwi`,
  `Waveshare-ESP32-S3`, `Supervisor`, `FlowConnectDisplay`, `Micronova` et
  `SupervisorWokwi` de l'ancien `platformio.ini`;
- profils `src/Profiles/{FlowIO,Supervisor,FlowConnectDisplay,Micronova}`;
- cartes `FlowIODINBoard.h`, `SupervisorBoardRev1.h` et `MicronovaBoard.h`;
- domaine `src/Domain/Supervisor`;
- modules Micronova, FlowConnectDisplay, I2C Config client/serveur et ancien
  `SupervisorHMIModule`;
- bibliothèque `lib/ESP32_Flasher`;
- répertoire et fichiers Wokwi, scripts associés et diagrammes racine;
- partitions FlowIO/Supervisor et partition Waveshare 8 Mo;
- anciens binaires suivis, index web générés, caches, sauvegardes et doublons;
- documentation propre aux architectures supprimées.

Le bitmap flow.io utilisé par le TFT a été déplacé dans `TFTModuleS3` avant la
suppression de l'ancien module HMI.

## Modifications fonctionnelles et de sécurité

- le bootstrap résout directement le profil, la carte et le domaine Waveshare;
- les macros de sélection de profil/carte et le `build_src_filter` ne sont plus
  nécessaires;
- les identifiants numériques des modules retirés restent réservés afin de ne
  pas décaler les identifiants persistés ou exposés;
- le générateur de valeurs par défaut n'est plus nommé Wokwi;
- les valeurs Wi-Fi et MQTT par défaut sont vides, et MQTT est désactivé jusqu'à
  sa configuration dans l'interface/NVS;
- l'entité Home Assistant « Supervisor IP », sans objet sur la carte unique, a
  été retirée;
- les scripts d'export ne connaissent plus que la cible Waveshare.

Le protocole HMI UDP et le contrat `IFlowCfg` ont été conservés. Ils sont encore
référencés par les fonctions optionnelles de l'interface web Waveshare; les
retirer imposerait une refonte applicative hors du périmètre de cette
spécialisation.

## Validation

Commande exécutée:

```sh
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3
```

Dernier résultat:

- statut: succès;
- RAM: 106 828 / 327 680 octets (32,6 %);
- flash applicatif: 2 060 114 / 6 553 600 octets (31,4 %);
- image totale annoncée par l'éditeur de liens: 2 060 370 octets.

Cette validation couvre la génération des métadonnées, la préparation SPIFFS,
la compilation, l'édition de liens et la création de l'image ESP32-S3. Elle ne
remplace pas un essai sur la carte réelle.

## Risques résiduels

- PlatformIO décrit la carte de base `freenove_esp32_s3_wroom` comme une N8R8
  de 8 Mo, tandis que le projet conserve la table historique
  `partition_waveshare_ota_16mb.csv`. Confirmer la référence exacte du module et
  la taille flash détectée avant une mise à jour OTA en production.
- Tester sur matériel le W5500, les expanders I2C, OneWire, TFT/Nextion, RTC,
  relais, entrées isolées, provisioning, MQTT et OTA.
- Un équipement ayant déjà des identifiants Wi-Fi/MQTT en NVS les conservera;
  une carte neuve ou effacée démarrera sans broker MQTT configuré.
- Le dépôt source exporté ne contenait pas de licence. Le dépôt spécialisé
  contient désormais le fichier [`LICENSE`](LICENSE).

## Création initiale du dépôt local

Lors de la migration, l'arborescence a été copiée vers un dossier neuf puis
initialisée avec Git. Ces étapes sont conservées ici uniquement à titre
historique : le dépôt actuel est déjà initialisé.

```sh
cd /chemin/vers/le-nouveau-dossier
git init
git add .
git commit -m "Initial Waveshare ESP32-S3 import"
```

Les caches `.pio/` et `data/wc/` restent des sorties générées. Les binaires
exportés sont produits par le script post-build conformément à la politique du
dépôt actuel.
