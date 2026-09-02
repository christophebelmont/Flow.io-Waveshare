# Nomenclature matérielle

Les nomenclatures distinguent les composants des PCB, les modules enfichables, le câblage et les accessoires optionnels.

## Fichiers de référence

| Fichier | Contenu |
|---|---|
| [`system.csv`](../../hardware/bom/system.csv) | liste d'achat globale : contrôleur, Companion, alimentation, capteurs, actionneurs, afficheurs et options |
| [`companion-v1.1.csv`](../../hardware/bom/companion-v1.1.csv) | connecteurs et modules nécessaires à la Companion v1.1 |
| [`led-panel.csv`](../../hardware/bom/led-panel.csv) | panneau frontal PCF8574A et LEDs |
| [`ph-orp-board-v2.csv`](../../hardware/bom/ph-orp-board-v2.csv) | composants de la carte pH/ORP V2 |

## Niveaux de nécessité

| Statut | Signification |
|---|---|
| `Required` | nécessaire au sous-ensemble concerné |
| `Recommended` | recommandé pour la configuration Flow.io de référence |
| `Optional` | extension indépendante |
| `Alternative` | choix exclusif au sein d'une solution, notamment pour pH/ORP |
| `Third-party` | composant provenant d'un projet externe ; vérifier sa référence avant commande |

## Ensemble minimal

Pour construire l'installation de référence sans ses options d'affichage, prévoir au minimum :

- un Waveshare ESP32-S3-ETH-8DI-8RO, avec ou sans PoE ;
- un PCB Companion v1.1 et les composants `Required` de sa BOM ;
- un ADS1115 **16 bits**, un INA226 et un convertisseur de niveau I2C 5 V / 3,3 V ;
- quatre nappes 7 broches femelle-femelle et une nappe 10 broches mâle-femelle ;
- deux sondes DS18B20, eau et air ;
- les capteurs et actionneurs correspondant aux fonctions effectivement activées ;
- une solution d'alimentation et de protection adaptée ;
- une des deux chaînes d'acquisition pH/ORP si ces mesures sont utilisées.

## Prix et liens d'achat

Les prix proviennent d'une liste datée d'août 2026. Ils servent uniquement d'ordre de grandeur. La devise, lorsqu'elle n'était pas explicitement indiquée, est marquée **(À confirmer)** dans `system.csv`.

Un lien marchand n'impose pas un fournisseur. Avant de commander :

1. comparer la référence fabricant, le boîtier, le pas et l'orientation ;
2. vérifier que l'ADS est bien un ADS1115 16 bits et non un ADS1015 12 bits ;
3. vérifier la tension logique et la tension d'alimentation de chaque module ;
4. contrôler les quantités de conditionnement, différentes des quantités réellement montées ;
5. si un lien n'est plus disponible, choisir une référence techniquement équivalente et mettre la BOM à jour.

## Informations à confirmer

- tension, débit, tubulure et connectique des deux pompes péristaltiques ;
- modèles exacts des sondes pH et ORP, longueurs de câble et conditions d'immersion ;
- devise des prix historiques ;
- résistance des réseaux `R3` et `R4` du panneau LED ;
- modèle et longueur du câble HY2.0 du panneau LED ;
- adresse configurée de l'ADS1115 sur la carte pH/ORP V2 ;
- licence de redistribution de la carte pH/ORP V2 ;
- référence actuelle du boîtier DIN RS PRO 1862291 et des boîtiers Nextion 5/7 pouces.
