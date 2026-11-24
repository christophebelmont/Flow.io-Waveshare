# Changelog — Sécurisation de la mise à jour Nextion

**Date :** 24 novembre 2026  
**Projet :** Flow.io-Waveshare (ESP32-S3)  
**Branche source :** `stephdust/Flow.io-Waveshare:fix/nextion-psram-uart-priority-20261124`  
**Cible de PR :** `christophebelmont/Flow.io-Waveshare:dev`

## Contexte et incident

La mise à jour du firmware Nextion échouait à l'étape `flashing` avec `get baudrate error`. Un des redémarrages observés portait `reset_reason: 7` (watchdog). L'ancien outil de programmation Nextion utilisait `Serial1`, affecté au RS485 Modbus, alors que le pilote HMI normal utilise `Serial2`. L'interface web pouvait afficher un **5 % artificiel** même sans progression réelle du transfert.

## Modifications apportées

### 1. Préchargement et vérification en PSRAM

- Téléchargement de l'**intégralité du fichier `.tft` en PSRAM** avant toute modification du Nextion ; aucun repli vers un transfert HTTP → UART simultané.
- Réservation d'au moins **2 Mio de PSRAM libre** et vérification de la disponibilité d'un bloc contigu suffisant.
- Jusqu'à **trois tentatives HTTP au total** ; reprise par `Range` seulement si la réponse `206 Content-Range` est cohérente, sinon redémarrage propre en cas de `200`.
- Contrôle de la taille et vérification **SHA-256 obligatoire** à partir du manifeste publié, avant activation de la programmation série.

### 2. Fiabilisation de la liaison Nextion

- Le chargeur `ESPNexUpload` utilise désormais **`Serial2` (UART HMI)** et n'interfère plus avec `Serial1` (RS485).
- Réduction des boucles d'attente actives avec coopération FreeRTOS afin de limiter le risque watchdog.
- Vérification des écritures UART et des acquittements `0x05` à chaque bloc de 4 Kio, **y compris le dernier**.
- En cas d'échec du transfert : pas de redémarrage logiciel forcé d'un écran potentiellement incomplet ; libération des ressources et remontée de l'erreur.

### 3. Priorité et exclusivité des mises à jour

- **Une seule mise à jour à la fois** : Nextion et OTA ESP32-S3 **firmware + SPIFFS** (locale ou distante) s'excluent mutuellement dès la mise en file d'attente.
- Les tentatives concurrentes, vérifications de manifeste ou demandes d'annulation obsolètes ne peuvent pas perturber l'opération en cours.
- Suspension des nouvelles communications **Modbus TCP** pendant la mise à jour.
- Le séquenceur **Modbus RTU/RS485** cesse d'accepter de nouvelles transactions, tout en laissant terminer l'échange déjà engagé ; reprise du service après libération du verrou.

### 4. Progression réelle dans l'interface web FR/EN

Les étapes sont identifiées séparément :

1. Téléchargement du fichier `.tft` en PSRAM (octets reçus).
2. Vérification SHA-256.
3. Initialisation UART Nextion.
4. Transfert série (octets envoyés).
5. Attente du dernier acquittement.

Le pourcentage global est une **pondération des étapes mesurées**, pas une estimation du temps restant. Les faux pourcentages minimums de 5 % au démarrage et en cas d'échec Nextion ont été supprimés.

## Validation

- **Mise à jour Nextion réussie sur le matériel de l'utilisateur, le 24/11/2026.**
- **4 495 scénarios déterministes simulés**, sans échec dans les modèles : PSRAM, HTTP, UART, concurrence, progression, reprise Modbus.
- **44 contrôles statiques** du code et des interfaces vérifiés lors de la préparation.
- Les simulations ne remplacent pas les essais matériels des scénarios dégradés ; **la revue de Christophe, les tests OTA firmware+SPIFFS en concurrence et les essais sur les autres modèles d'écran restent recommandés**.

## Périmètre et compatibilité

**Aucune modification du protocole Modbus ou des données métier.** La suspension des transactions est temporaire et liée à l'activité de l'outil de mise à jour. Les fichiers de configuration, la topologie MQTT/HA et les calculs ne sont pas modifiés. La génération standard des manifestes Nextion fournit déjà les empreintes SHA-256 demandées par le nouveau contrôle.

## Fichiers principaux

- `src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp/.h`
- `lib/ESPNexUpload/src/ESPNexUpload.cpp/.h`
- `src/Modules/IOModule/IOModule.cpp/.h`
- `data/webinterface/app.js` et `data/webinterface/i18n/{fr,en}.json`
- `scripts/tests/check_nextion_ota_priority.mjs`
- `scripts/tests/simulate_nextion_ota_failure_matrix.mjs`
- `docs/core/nextion-psram-ota-failure-matrix-2026-11-24.md`

**Remarque :** cette PR est distincte des autres changements en attente, notamment la famille 5.
