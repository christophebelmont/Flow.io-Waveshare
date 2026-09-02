# Profils, cartes, domaines et bootstrap

Cette page décrit comment l'unique firmware `Flowio-waveshare-esp32-s3` est composé à partir du profil Waveshare, de sa carte et du domaine Pool.

## Vue d'ensemble

```text
platformio.ini
                 |
                 v
Profiles::Waveshare
  |-- BoardCatalog::activeBoard() -> WaveshareBoard
  |-- DomainCatalog::pool()       -> PoolDomain
  `-- WaveshareBootstrap          -> modules et services runtime
```

## Couche `App`

Références principales:

- `src/App/Bootstrap.cpp`
- `src/App/AppContext.h`
- `src/App/FirmwareProfile.h`
- `src/App/BuildFlags.h`

`Bootstrap::run()` utilise directement `Profiles::Waveshare::profile()`, crée le contexte global `AppContext`, installe la carte, le domaine et l'identité produit, puis appelle le setup du profil.

## Couche `Board`

Références principales:

- `src/Board/WaveshareBoard.h`
- `src/Board/BoardCatalog.cpp`
- `src/Board/BoardSpec.h`

`WaveshareBoard.h` définit le matériel compilé :

- UART de logs et d'HMI;
- bus I2C et 1-Wire;
- Ethernet W5500;
- buzzer et TFT ST7789;
- GPIO et points IO;
- capacités IO, MQTT et Home Assistant.

Les broches de bus et périphériques sont des constantes de build. Certains champs sont recopiés comme valeurs initiales de modules puis peuvent être remplacés par une configuration NVS existante; les commentaires de `WaveshareBoard.h` indiquent ce comportement champ par champ.

## Couche `Domain`

Références principales:

- `src/Domain/Pool/PoolDomain.h`
- `src/Domain/Pool/PoolIds.h`
- `src/Domain/DomainSpec.h`

Le domaine Pool décrit les rôles fonctionnels indépendamment du câblage physique:

- 13 slots capteurs Waveshare;
- 8 presets d'équipements piscine;
- 21 liaisons `domain_slot -> io_slot`;
- les valeurs par défaut de la logique piscine.

Le domaine ne sélectionne pas directement un GPIO ou un canal d'expander. Il pointe vers un IO slot stable; le profil IO choisit ensuite son binding port. Voir la [cartographie Waveshare](waveshare-io-map.md).

## Couche `Profiles::Waveshare`

Références principales:

- `src/Profiles/Waveshare/WaveshareProfile.cpp`
- `src/Profiles/Waveshare/WaveshareProfile.h`
- `src/Profiles/Waveshare/WaveshareModuleInstances.cpp`
- `src/Profiles/Waveshare/WaveshareBootstrap.cpp`
- `src/Profiles/Waveshare/WaveshareIoLayout.h`
- `src/Profiles/Waveshare/WaveshareIoAssembly.cpp`

Le profil assemble `WaveshareBoard`, `PoolDomain`, l'identité MQTT/runtime et toutes les instances de modules. Son bootstrap:

1. initialise la console et la politique PSRAM;
2. enregistre les modules Core, réseau, UI et métier;
3. configure les binding ports, expanders et IO slots;
4. définit les `PoolDevice` à partir du domaine;
5. enregistre les providers runtime MQTT et Home Assistant;
6. lance le cycle de vie via `ModuleManager`.

### Responsabilités principales

| Zone | Modules ou services |
|---|---|
| Core | logs, `ConfigStore`, `DataStore`, commandes, `EventBus` |
| Réseau | Ethernet, Wi-Fi, provisioning, web, MQTT |
| Exploitation | mise à jour firmware, temps/RTC, alarmes, supervision système |
| HMI | UDP/série, buzzer, TFT local |
| Piscine | IO, logique Pool, équipements Pool, Home Assistant |

## Les trois niveaux de binding

| Couche | Structure source | Exemple filtration |
|---|---|---|
| Domaine | `DomainIoSlotBinding` dans `PoolDomain.h` | `ActuatorFiltrationPump -> d00` |
| Endpoint | `IODigitalOutputDefinition` dans `WaveshareIoAssembly.cpp` | `d00`, actif haut, état initial OFF |
| Matériel | `IOBindingPortSpec` dans `WaveshareIoLayout.h` | port `300`, `EXIO1`, TCA9554 bit 0 |

Cette séparation permet de changer l'affectation physique d'un IO slot sans modifier le code métier qui consomme le domain slot.

## Sélection par `platformio.ini`

L'environnement unique est `[env:Flowio-waveshare-esp32-s3]`. Il active notamment:

- Ethernet W5500, RTC PCF85063 et TFT S3;
- PSRAM et les capacités propres à la carte;
- les scripts de génération et d'export de binaires.

Commande de référence:

```sh
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3
```

Il n'existe pas de variante de simulation ou de seconde cible dans ce dépôt.

## Où modifier quoi

| Type de modification | Zone principale |
|---|---|
| broche, bus ou périphérique matériel | `src/Board/WaveshareBoard.h` |
| rôle métier ou liaison domaine vers IO slot | `src/Domain/Pool/*` |
| binding port ou valeur IO par défaut | `src/Profiles/Waveshare/WaveshareIoLayout.h` |
| création des endpoints et intégration HA | `src/Profiles/Waveshare/WaveshareIoAssembly.cpp` |
| modules présents et ordre d'enregistrement | `src/Profiles/Waveshare/*` |
| flags, dépendances et scripts de build | `platformio.ini` |
