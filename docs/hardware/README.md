# Matériel flow.io

Flow.io s'appuie sur un contrôleur Waveshare ESP32-S3-ETH-8DI-8RO et une carte Companion v1.1. Vous trouverez ici les composants nécessaires, les fichiers de fabrication, le montage et le raccordement au firmware.

![Installation flow.io avec contrôleur Waveshare, Companion et afficheurs](assets/overview/flowio-installation.jpg)

## Parcours recommandé

1. choisir le [contrôleur Waveshare](waveshare-controller.md) et son mode d'alimentation ;
2. préparer la [nomenclature générale](bill-of-materials.md) ;
3. commander ou fabriquer la [Companion v1.1](companion-v1.1.md) ;
4. suivre le [guide d'assemblage de la Companion](companion-assembly.md) ;
5. intégrer, si nécessaire, le [boîtier DIN et le TFT local](enclosures-and-local-tft.md) ;
6. installer un [écran Nextion](nextion-displays.md) et/ou les [capteurs et extensions](sensors-and-extensions.md) ;
7. choisir l'[interface pH/ORP](ph-orp-interface.md) ;
8. terminer par la [mise en service du firmware](../integration/mise-en-service.md).

## Composition du système

| Sous-ensemble | Rôle | Statut |
|---|---|---|
| Waveshare ESP32-S3-ETH-8DI-8RO | calcul, réseau, relais, entrées isolées et alimentation | requis |
| Flow.io Companion v1.1 | adaptation et distribution des signaux vers les capteurs et interfaces | requis avec le câblage présenté ici |
| ADS1115 16 bits | acquisition analogique pH, ORP, pression et voie libre | requis |
| INA226 | mesure de tension, courant et puissance | requis dans le montage présenté |
| DS18B20 eau et air | températures OneWire | requis pour les fonctions de référence |
| Nextion | interface tactile locale | recommandé |
| TFT ST7789 240×320 | affichage local intégré au boîtier Companion | optionnel |
| MCP23017 | entrées et sorties supplémentaires | recommandé |
| PIR, ENV-IV, ENV-Pro, LED, Venice | extensions locales | optionnel |
| Adaptateurs pH/ORP | conditionnement et isolation des sondes | requis si les fonctions de régulation pH/ORP sont utilisées |

## Description du système

- La [cartographie IO](../core/waveshare-io-map.md) indique les broches, les adresses I2C et les affectations logiques.
- [`hardware/catalog.yaml`](../../hardware/catalog.yaml) répertorie les versions des fichiers de fabrication et leurs sommes SHA-256.
- Les [BOM CSV](../../hardware/bom/) regroupent les composants et les références d'achat.
- La page [ESP / Nextion](../integration/nextion-esp-protocol.md) décrit les échanges avec l'écran tactile.

La mention **(À confirmer)** signale une information qui doit encore être vérifiée. Elle ne constitue pas une valeur par défaut.

## Sécurité

> Les relais et alimentations peuvent être raccordés au secteur. Couper, condamner et vérifier l'absence de tension avant toute intervention. Le raccordement secteur, la protection contre les surintensités, la section des conducteurs, la mise à la terre et l'enveloppe doivent être définis par une personne qualifiée selon les règles applicables à l'installation.

Les essais initiaux doivent être réalisés sans charge secteur : contrôler d'abord les continuités, l'absence de court-circuit, les polarités et les tensions 3,3 V / 5 V, puis tester les entrées et relais avec des circuits adaptés.
