# Contrôleur Waveshare ESP32-S3-ETH-8DI-8RO

Le profil matériel de flow.io cible les modèles `ESP32-S3-ETH-8DI-8RO` et `ESP32-S3-POE-ETH-8DI-8RO`. Les variantes `-C` équipées d'une interface CAN ne font pas partie du profil actuel : leur compatibilité est **(À confirmer)**.

![Contrôleur Waveshare ESP32-S3-POE-ETH-8DI-8RO](../pictures/waveshare-esp32-s3-poe-eth-8di-8ro.png)

## Choisir la variante

| Variante | Ethernet | PoE | Alimentation alternative |
|---|---|---|---|
| ESP32-S3-ETH-8DI-8RO | oui | non | USB-C 5 V / 1 A ou bornier 7–36 VDC |
| ESP32-S3-POE-ETH-8DI-8RO | oui | oui | USB-C 5 V / 1 A ou bornier 7–36 VDC |

La variante PoE simplifie le coffret lorsque le réseau fournit une alimentation PoE compatible. La classe et le budget PoE exacts de l'équipement réseau utilisé sont **(À confirmer)** avant installation.

Sources constructeur : [page produit](https://www.waveshare.com/product/esp32-s3-eth-8di-8ro.htm) et [wiki technique](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO).

## Ressources utilisées par flow.io

| Ressource | Affectation |
|---|---|
| W5500 Ethernet | GPIO13 MOSI, 14 MISO, 15 SCLK, 16 CS, 12 INT, 39 RST |
| Bus I2C partagé | GPIO42 SDA, GPIO41 SCL, 400 kHz |
| UART Nextion | UART2, RX44, TX43, 115200 bauds |
| OneWire eau | GPIO20 |
| OneWire air | GPIO19 |
| Entrées utilisateur | GPIO5 à GPIO11 |
| Reset usine flow.io | GPIO4, actif bas, appui continu de 5 s |
| Relais | EXIO1 à EXIO8 via TCA9554 `0x20` |
| LED RGB intégrée | GPIO38 |
| Buzzer | GPIO46 |

Le matériel Waveshare expose huit entrées numériques GPIO4 à GPIO11. Dans flow.io, GPIO4 est réservé au bouton de remise à zéro et n'est donc pas disponible comme entrée configurable. La [cartographie IO](../core/waveshare-io-map.md) détaille les ports utilisables.

## Relais et charges

Waveshare spécifie huit relais, chacun donné pour un maximum de 10 A sous 250 VAC ou 30 VDC. Cette valeur est une limite de contact constructeur et ne dispense pas du dimensionnement en fonction de la charge réelle, de son courant d'appel, de la température du coffret et des protections en amont.

Par défaut, les relais commandent :

| Relais | Fonction par défaut |
|---|---|
| EXIO1 | filtration |
| EXIO2 | pompe pH |
| EXIO3 | pompe désinfectant / chlore |
| EXIO4 | robot |
| EXIO5 | remplissage |
| EXIO6 | électrolyseur |
| EXIO7 | éclairage |
| EXIO8 | chauffage |

## Raccordement de la Companion

La Companion reprend les alimentations et signaux du connecteur 28 broches au moyen de quatre nappes 7 broches et d'une nappe 10 broches. Les connexions 7 broches sont disposées en miroir pour limiter les croisements ; respecter les numéros sérigraphiés et ne jamais se fier uniquement à la couleur des conducteurs.

Avant la première mise sous tension :

1. débrancher USB, PoE, alimentation DC et charges des relais ;
2. vérifier l'orientation et le décalage de chaque connecteur ;
3. contrôler la continuité des masses et l'absence de court-circuit entre 3,3 V, 5 V et GND ;
4. régler les cavaliers d'alimentation de la Companion ;
5. remettre sous tension sans charge secteur et mesurer les rails.
