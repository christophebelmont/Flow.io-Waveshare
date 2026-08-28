# Documentation technique de flow.io

Cette documentation s'adresse aux personnes qui installent le matériel, compilent le firmware, adaptent les raccordements ou interviennent sur le logiciel. La présentation fonctionnelle destinée aux utilisateurs reste disponible dans le [README principal](../README.md).

Le dépôt contient une seule cible matérielle et un seul environnement PlatformIO : `Flowio-waveshare-esp32-s3`.

## Démarrage rapide

Prérequis : [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html).

Compiler le firmware :

```sh
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3
```

Téléverser le firmware et ouvrir le moniteur série :

```sh
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3 -t upload
~/.platformio/penv/bin/pio device monitor -b 115200
```

Les dépendances déclarées dans `platformio.ini` sont installées automatiquement par PlatformIO. Les identifiants Wi-Fi et MQTT ne sont pas inscrits dans le code source : ils sont renseignés lors du provisioning ou depuis l'interface de configuration, puis conservés en NVS.

## Parcours recommandé

### Installer et raccorder

- [Mise en service du profil Waveshare](integration/mise-en-service.md)
- [Cartographie complète des entrées, sorties et raccordements](core/waveshare-io-map.md)
- [Protocole entre l'ESP32 et l'écran Nextion](integration/nextion-esp-protocol.md)

### Comprendre le dépôt et l'architecture

- [Structure du dépôt et responsabilités des répertoires](core/repository-structure.md)
- [Architecture générale](core/architecture.md)
- [Profils, carte, domaine et bootstrap](core/profiles-board-domain-app.md)
- [Services Core](core/services.md)
- [Modèle ConfigStore, DataStore, EventBus et MQTT](core/data-event-model.md)
- [Exposition des données dans l'interface Runtime UI](core/runtime-ui-exposure.md)
- [Chargement modulaire des ressources web](core/webinterface-assets-modular.md)
- [Logique métier de la piscine](integration/flowio-poollogic-business.md)

### Intégrer et diagnostiquer

- [Topologie et conventions MQTT](core/mqtt-topics.md)
- [Matrice de qualité des modules](core/module-quality-gates.md)
- [Historique de la spécialisation Waveshare](../MIGRATION.md)

## Repères d'architecture

Le démarrage suit une chaîne volontairement explicite :

```text
src/main.cpp
  -> App::Bootstrap
  -> profil Waveshare
  -> description de la carte et domaine Pool
  -> enregistrement des modules et des services
  -> boucle d'exécution
```

Les responsabilités sont séparées en quatre niveaux :

| Niveau | Responsabilité | Emplacement principal |
|---|---|---|
| Carte | broches, bus, périphériques et capacités statiques | `src/Board/` |
| Domaine | rôles et comportements propres à la piscine | `src/Domain/Pool/` |
| Profil | assemblage matériel/logiciel de la cible Waveshare | `src/Profiles/Waveshare/` |
| Modules | fonctions autonomes exposées par services et événements | `src/Modules/` |

La chaîne d'affectation des entrées et sorties est :

```text
domain_slot (fonction piscine) -> io_slot (point logique) -> binding_port (ressource physique)
```

La [cartographie IO](core/waveshare-io-map.md) contient les valeurs par défaut et les ressources réservées.

## Référence par module

### Infrastructure et stockage

- [LogHubModule](modules/LogHubModule.md)
- [LogDispatcherModule](modules/LogDispatcherModule.md)
- [LogSerialSinkModule](modules/LogSerialSinkModule.md)
- [LogAlarmSinkModule](modules/LogAlarmSinkModule.md)
- [EventBusModule](modules/EventBusModule.md)
- [ConfigStoreModule](modules/ConfigStoreModule.md)
- [DataStoreModule](modules/DataStoreModule.md)
- [CommandModule](modules/CommandModule.md)
- [SystemModule](modules/SystemModule.md)
- [SystemMonitorModule](modules/SystemMonitorModule.md)

### Réseau, temps et interfaces

- [WifiModule](modules/WifiModule.md)
- [TimeModule](modules/TimeModule.md)
- [MQTTModule](modules/MQTTModule.md)
- [HAModule](modules/HAModule.md)
- [HMIModule](modules/HMIModule.md)

### Piscine et entrées/sorties

- [AlarmModule](modules/AlarmModule.md)
- [IOModule](modules/IOModule.md)
- [PoolLogicModule](modules/PoolLogicModule.md)
- [PoolDeviceModule](modules/PoolDeviceModule.md)

La [matrice de qualité](core/module-quality-gates.md) complète ces fiches pour les modules d'infrastructure qui ne disposent pas encore d'une page dédiée.

## Fichiers générés

La compilation prépare automatiquement les métadonnées de configuration, le manifeste Runtime UI, les ressources SPIFFS et les binaires exportés. Les sorties de `.pio/`, `data/wc/` et `binary/` ne constituent pas la source de référence : leurs générateurs sont dans `scripts/` et leurs entrées versionnées se trouvent notamment dans les dossiers `text/` des modules et dans `data/webinterface/`.

Avant de modifier directement un fichier généré, consulter la [structure du dépôt](core/repository-structure.md) pour identifier sa source.
