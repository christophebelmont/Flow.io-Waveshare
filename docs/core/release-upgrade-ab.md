# Releases A/B et packages d’upgrade

La cible Waveshare utilise deux slots de release indissociables : `app0` avec
`spiffs0`, et `app1` avec `spiffs1`. Le slot courant est toujours déduit de la
partition applicative réellement exécutée. Le montage SPIFFS utilise ensuite le
label associé; aucun état NVS indépendant ne choisit le filesystem.

## Partitionnement 16 Mio

| Partition | Offset | Taille | Rôle |
|---|---:|---:|---|
| `nvs` | `0x009000` | `0x20000` (128 Kio) | paramètres persistants |
| `otadata` | `0x029000` | `0x2000` (8 Kio) | sélection OTA |
| `app0` | `0x030000` | `0x630000` (6,1875 Mio) | application slot A |
| `app1` | `0x660000` | `0x630000` (6,1875 Mio) | application slot B |
| `spiffs0` | `0xc90000` | `0x180000` (1,5 Mio) | assets slot A |
| `spiffs1` | `0xe10000` | `0x180000` (1,5 Mio) | assets slot B |
| `runtime` | `0xf90000` | `0x60000` (384 Kio) | journal mutable partagé |

Chaque slot applicatif dispose de 6,1875 Mio, avec une marge confortable pour le
firmware actuel. L’image SPIFFS a une taille fixe de 1,5 Mio. Le journal d’activité
est séparé des releases afin qu’une mise à jour ne l’efface pas.

Seul `spiffs0` utilise le sous-type CSV reconnu par la cible PlatformIO
`uploadfs`; `spiffs1` utilise le sous-type privé `0x41`. Le runtime ESP-IDF monte
les deux partitions par leur label explicite avec le sous-type `ANY`. Cette
distinction garantit qu’une première installation USB écrit bien `spiffs0`, sans
laisser le choix du slot au comportement implicite de l’outil de build.

## Package local

Le build de l’environnement `Flowio-waveshare-esp32-s3` exporte dans `binary/` :

- `flowios3-<version>.bin`;
- `flowios3-spiffs-<version>.bin`;
- `flowio-<version>.zip`.

Le ZIP contient exactement `manifest.json`, `firmware.bin` et `spiffs.bin`.
Les entrées binaires sont stockées sans compression afin que le navigateur les
transmette directement par tranches de `Blob`, sans charger les deux images en
mémoire. Le manifeste versionné porte les tailles et SHA-256 des deux images.

L’interface commence une transaction unique, écrit d’abord `spiffs` puis le
firmware dans le slot inactif, vérifie taille, SHA-256 et fichiers Web essentiels,
puis seulement sélectionne la nouvelle partition de boot. Une interruption avant
le commit laisse la release courante intacte.

Au premier démarrage, le firmware monte le SPIFFS associé. L’application OTA
n’est marquée valide qu’après la fin du démarrage des modules et la validation de
`release.json` et des assets essentiels. Si ce contrôle échoue, le rollback OTA
ramène automatiquement à l’autre couple application/filesystem.

## Première installation USB

Le passage à cette table de partitions est volontairement une opération USB. Il
faut flasher le firmware, la table et `spiffs0`; aucune migration OTA depuis
l’ancien partitionnement n’est prévue. Avec PlatformIO, la cible `upload` installe
l’application et la table, puis la cible `uploadfs` installe l’image de release
dans `spiffs0`.

Le mécanisme historique d’upgrade depuis le serveur reste disponible. Lors d’un
upgrade firmware seul, le SPIFFS courant est recopié vers le slot inactif avant
l’écriture de l’application, ce qui garantit au minimum un slot Web exploitable.
L’upgrade SPIFFS historique conserve son comportement en place sur le slot actif;
le package ZIP local est le chemin transactionnel garantissant le couple complet.

## Réinstallation sans conservation des réglages (NVS 128 Kio)

La NVS passe de 20 à 128 Kio. Les deux applications restent de taille égale ;
les emplacements des systèmes de fichiers et du coredump sont conservés.
L’image d’initialisation OTA `boot_app0.bin` doit être écrite à `0x29000` :
`board_upload.arduino.boot_app0` fixe cet emplacement dans PlatformIO. L’ancienne
adresse Arduino par défaut `0xe000` se trouve désormais dans la NVS et ne doit
plus être utilisée pour cette image. PlatformIO déduit l’adresse applicative
`0x30000` de la table.

Pour une installation neuve explicitement choisie sans conservation des données,
connecter la carte en USB puis exécuter, depuis la racine du projet :

```sh
pio run -e Flowio-waveshare-esp32-s3 -t erase
pio run -e Flowio-waveshare-esp32-s3 -t upload
pio run -e Flowio-waveshare-esp32-s3 -t uploadfs
```

L’effacement complet supprime réglages, identifiants réseau, compteurs, historiques
et journaux locaux. Reconfigurer le réseau et MQTT après l’installation.
Si plusieurs cartes sont branchées, ajouter `--upload-port PORT` à chaque commande.
Installer les deux images avant de vérifier le démarrage ; la validation de release
requiert les fichiers web. Le ZIP OTA seul ne met pas à jour la table de partitions.

Au redémarrage, vérifier la connexion réseau, l’état MQTT et la sauvegarde d’un
réglage après un second reboot. Le journal d’occupation NVS doit refléter la nouvelle
partition et ne plus montrer d’échec d’écriture `0x1105`.
