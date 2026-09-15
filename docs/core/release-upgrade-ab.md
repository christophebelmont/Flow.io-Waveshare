# Releases A/B et packages d’upgrade

La cible Waveshare utilise deux slots de release indissociables : `app0` avec
`spiffs0`, et `app1` avec `spiffs1`. Le slot courant est toujours déduit de la
partition applicative réellement exécutée. Le montage SPIFFS utilise ensuite le
label associé; aucun état NVS indépendant ne choisit le filesystem.

## Partitionnement 16 Mio

| Partition | Offset | Taille | Rôle |
|---|---:|---:|---|
| `app0` | `0x010000` | `0x640000` (6,25 Mio) | application slot A |
| `app1` | `0x650000` | `0x640000` (6,25 Mio) | application slot B |
| `spiffs0` | `0xc90000` | `0x180000` (1,5 Mio) | assets slot A |
| `spiffs1` | `0xe10000` | `0x180000` (1,5 Mio) | assets slot B |
| `runtime` | `0xf90000` | `0x60000` (384 Kio) | journal mutable partagé |

Le firmware 2.0.1 compilé occupe environ 2,19 Mo, soit 33,4 % d’un slot
applicatif. L’image SPIFFS a une taille fixe de 1,5 Mio. Le journal d’activité
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
