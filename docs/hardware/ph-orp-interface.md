# Interfaces pH et ORP

Les sondes pH et ORP produisent des signaux de très forte impédance qui ne doivent pas être raccordés directement à l'ADS1115 de la Companion. Une chaîne d'amplification et d'isolation adaptée est requise.

## Deux solutions documentées

| Solution | Chaîne de mesure | Avantages | Points d'attention |
|---|---|---|---|
| Adaptateurs analogiques | sonde BNC → adaptateur Phidgets 1130 → isolateur DFR0504 → ADS1115 interne | composants disponibles séparément, diagnostic par étage | deux adaptateurs, deux isolateurs et davantage de câblage |
| Carte pH/ORP V2 | deux sondes BNC → amplificateurs LMP7721 → ADS1115 → isolateur I2C ADM3260 | deux voies et isolation réunies sur une carte | source tierce, adresse I2C et licence **(À confirmer)** |

## Solution analogique séparée

Prévoir par voie :

- une sonde pH ou ORP avec connecteur BNC ;
- un adaptateur Phidgets 1130 ou équivalent ;
- un câble analogique compatible ;
- un module d'isolation DFR0504 ou équivalent ;
- un raccordement vers l'ADS1115 interne de la Companion.

La configuration de référence utilise :

| Mesure | ADS1115 interne | Binding port | IO slot |
|---|---|---:|---|
| ORP | A0 | 100 | `a00` |
| pH | A1 | 101 | `a01` |

L'ordre, l'échelle et la polarité de la sortie analogique doivent être vérifiés sur les modules effectivement achetés.

## Carte pH/ORP V2

La BOM disponible décrit deux amplificateurs `LMP7721MA/NOPB`, un convertisseur `ADS1115IDGSR` et un isolateur I2C `ADM3260`. La [BOM normalisée](../../hardware/bom/ph-orp-board-v2.csv) conserve les 18 lignes du fichier source.

Le projet amont fournit BOM, Gerber, schéma PDF, sources Altium et Pick & Place dans [Gixy31/ESP32-PoolMaster](https://github.com/Gixy31/ESP32-PoolMaster/tree/main/pH_Orp%20Board%20V2). Le dépôt amont renvoie également au [projet original de Loïc](https://github.com/Loic74650/pH_Orp_Board).

Ces fichiers ne sont pas recopiés dans flow.io parce que leur licence de redistribution est **(À confirmer)**. Le répertoire [`hardware/ph-orp-board-v2`](../../hardware/ph-orp-board-v2/) documente cette décision.

## Adresse I2C et intégration

Flow.io réserve `0x48` à l'ADS1115 interne et `0x49` à l'ADS1115 externe différentiel. L'adresse réellement configurée sur la carte pH/ORP V2 est **(À confirmer)**. Avant raccordement :

1. identifier les straps d'adresse de l'ADS1115 ;
2. vérifier l'adresse hors du bus principal ;
3. s'assurer qu'elle ne collisionne avec aucun autre composant ;
4. confirmer le brochage alimentation, SDA et SCL ;
5. contrôler que l'isolation galvanique n'est pas contournée par une masse ou une alimentation commune.

L'association directe de cette carte aux bindings différentiels `0x49` est **(À confirmer)** par un essai matériel documenté.

## Sondes et calibration

Le modèle exact des sondes, leur gamme, la matière de l'électrode ORP et la longueur de câble sont **(À confirmer)**. Pour une piscine, une électrode ORP en platine est généralement privilégiée aux consignes élevées, mais la compatibilité chimique doit être vérifiée auprès du fabricant.

Avant d'autoriser le dosage automatique :

- équilibrer l'eau et stabiliser la filtration ;
- calibrer la voie pH avec des solutions tampons appropriées ;
- contrôler la mesure ORP avec une solution de référence ;
- comparer les mesures à un instrument indépendant ;
- configurer les limites, temporisations et alarmes de dosage ;
- vérifier le sens et le débit réel de chaque pompe.

La calibration logicielle des entrées est décrite dans [IOModule](../modules/IOModule.md). Une mesure instable ou incohérente doit empêcher le dosage automatique jusqu'à résolution du défaut.
