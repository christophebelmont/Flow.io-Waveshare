# Écrans Nextion

Flow.io communique avec l'écran Nextion par l'UART2 à 115200 bauds. Les fichiers HMI et TFT fournis correspondent à la version `6.0.0`, également déclarée par `TFT_FIRMW=6.0.0` dans `platformio.ini`.

## Modèles fournis

| Modèle | Famille | Définition | Usage | Firmware 6.0.0 |
|---|---|---:|---|---|
| NX4832K035 | Enhanced | 480×320, 3,5 pouces | compact, mais interface plus petite | [`tft`](../../nextion/releases/6.0.0/FlowIO_Nextion_NX4832K035_011-6.0.0.tft) |
| NX8048P050-011R | Intelligent | 800×480, 5 pouces | modèle recommandé | [`tft`](../../nextion/releases/6.0.0/FlowIO_Nextion_NX8048P050_011-6.0.0.tft) |
| NX8048P070-011R | Intelligent | 800×480, 7 pouces | affichage plus grand | [`tft`](../../nextion/releases/6.0.0/FlowIO_Nextion_NX8048P070_011-6.0.0.tft) |

Les dessins dimensionnels officiels sont disponibles pour le [NX4832K035](https://cdn.nextion.tech/wp-content/uploads/2022/03/NX4832K035_dimension.pdf), le [NX8048P050-011R](https://cdn.nextion.tech/wp-content/uploads/2020/12/NX8048P050-011R-Dimension.pdf) et le [NX8048P070-011R](https://cdn.nextion.tech/wp-content/uploads/2022/03/NX8048P070-011R-Y_dimension.pdf).

## Sources éditables

| Définition | Source HMI |
|---|---|
| 480×320 | [`Flowio_Enhanced_480x320.HMI`](../../nextion/releases/6.0.0/Flowio_Enhanced_480x320.HMI) |
| 800×480 | [`Flowio_Intelligent_800x480.HMI`](../../nextion/releases/6.0.0/Flowio_Intelligent_800x480.HMI) |

Le 5 pouces et le 7 pouces utilisent deux binaires distincts compilés depuis la source 800×480. Ne pas renommer un binaire pour l'utiliser sur un autre modèle.

## Câblage

Le connecteur HMI de la Companion fournit `5V`, `TX`, `RX` et `GND`. La liaison utilise un câble XH2.54 4 broches inversé.

| Flow.io | Nextion |
|---|---|
| 5 V | 5 V |
| TX GPIO43 | RX |
| RX GPIO44 | TX |
| GND | GND |

La disposition physique peut varier selon le câble et le boîtier. Vérifier chaque conducteur par continuité avant de raccorder l'alimentation.

## Installer le firmware de l'écran

1. choisir le `.tft` qui correspond exactement au modèle imprimé sur l'écran ;
2. vérifier son empreinte dans [`hardware/catalog.yaml`](../../hardware/catalog.yaml) ;
3. transférer le fichier avec la procédure recommandée par Nextion, par carte microSD ou depuis Nextion Editor selon le modèle ;
4. couper puis rétablir l'alimentation de l'écran ;
5. retirer le support de transfert lorsque la mise à jour est terminée ;
6. raccorder l'écran à la Companion hors tension ;
7. vérifier les échanges série et la version affichée.

## Intégration logicielle

La configuration `hmi/nextion/enabled` active les écritures vers l'écran. Le PIR logique peut commander la veille et le réveil via `hmi/nextion/motion_io_id`.

Au démarrage, Flow.io envoie la commande `connect`, conserve le modèle complet
renvoyé par `comok` et calcule une clé de compatibilité. Les marqueurs tactiles
`R` et `C` ne participent pas à cette clé; `N` et les autres suffixes restent
distincts. Lors de la vérification des mises à jour, le firmware sélectionne
uniquement le TFT dont `display_compatibility` correspond à l'écran détecté,
puis choisit la version la plus récente. Aucun upgrade Nextion n'est accepté
sans cette sélection validée côté firmware.

Le contrat des pages, objets, opcodes et registres RTC est défini dans la [référence du protocole ESP / Nextion](../integration/nextion-esp-protocol.md). Les noms d'objets du fichier HMI font partie de ce contrat et ne doivent pas être modifiés sans adapter simultanément le firmware.
