# Journal d’intégration globale vers `christophebelmont/Flow.io-Waveshare:dev`

**Date : 22 novembre 2026**  
**Source :** `stephdust:experiment/modbus-tcp-psram-buffers-20261122`  
**Base initiale :** `c94670220cce243943c2bf858e13a2f5b00600c3`  
**Comparaison initiale :** 139 commits d'avance, 0 de retard, 67 fichiers. Ce journal constitue un supplément documentaire : son ajout à la branche augmente ensuite le nombre de commits et de fichiers.

## 1 — Transaction OTA A/B et migration historique

**Problème :** des mises à jour séparées firmware/SPIFFS peuvent produire une application incohérente avec son interface Web.

**Solution :** installer les deux images app/SPIFFS dans leurs partitions inactives, vérifier taille, SHA-256, version et métadonnées de manifeste, puis committer le couple seulement après validation des deux images et redémarrer une fois. Sur erreur, ne pas sélectionner le nouveau couple. Le chemin de migration d'une ancienne interface comporte d'abord un firmware de transition, puis la finalisation A/B via l'ancien contrôle SPIFFS. Les chemins ZIP local et Nextion restent distincts.

**Implémentation :** `FirmwareUpdateModule.cpp/.h`, `ReleaseStorage.cpp/.h`, `scripts/export_binaries.py`, `scripts/prepare_spiffs_data.py`. Détails : `docs/core/ota-two-step-legacy-migration-2026-11-20.md`.

## 2 — OTA PSRAM, pré-vérification SHA et repli

**Avant :** téléchargement HTTP et écriture Flash rapprochés par fragments de 1024 octets.

**Après :** l'image complète est téléchargée en PSRAM si un bloc contigu suffisant reste disponible **avec une réserve de 2 Mio**, la connexion est close, l'empreinte SHA-256 est calculée et vérifiée, puis la partition inactive est préparée et écrite par petits blocs de 1024 octets copiés dans un tampon interne. Une seule image est conservée intégralement à la fois. Le buffer PSRAM est libéré dans tous les chemins.

**Repli :** sans assez de PSRAM, reprendre le téléchargement/écriture streaming historique, journalisé `fallback_stream`. Ce repli est une décision mémoire avant le transfert ; un SHA invalide ou un échec Flash interrompt l'opération et ne déclenche pas le repli.

**Fichiers :** `src/Core/OtaPsramPolicy.h`, `FirmwareUpdateModule.cpp`, `docs/core/experimental-ota-psram-preverify-2026-11-21.md`.

## 3 — Politique HTTP : trois tentatives, reprise contrôlée

- Trois tentatives **au total** par manifeste/image pour les erreurs transitoires de transport ou HTTP 408/429/5xx.
- Sur transfert binaire partiel : `Range: bytes=<offset>-`, strict contrôle `206 Content-Range` et des tailles.
- Si le serveur renvoie un `200` complet, téléchargement redémarré à l'offset zéro.
- Échec immédiat sur réponse incohérente, hash erroné, erreur Flash ou HTTP permanent ; pas de rejeu automatique d'une écriture Flash ratée.
- La reprise HTTP n'est pas durable à travers un reboot.

**Fichiers :** `src/Core/OtaHttpRetryPolicy.h`, `FirmwareUpdateModule.cpp`, `scripts/tests/check_ota_http_retry.mjs`.

## 4 — Mémoire interne, PSRAM et budgets de piles

- Allocations ArduinoJson OTA explicites en PSRAM, sans redirection générale du tas.
- Réservation d'un `FrameStorage` Modbus TCP de **6 RX × 260 + 6 TX × 260 = 3120 octets** en PSRAM, une seule allocation réutilisée aux reconnexions.
- Si allocation impossible : aucun port serveur Modbus TCP n'est ouvert ; réessai périodique borné et journalisé.
- Piles FreeRTOS, gestion des sessions, lwIP/AsyncTCP, buffers critiques Flash restent internes.
- Stack OTA passée de **8192 à 10240 octets** et stack Modbus TCP de **4096 à 5120 octets** ; augmentation de consommation interne à distinguer du déplacement des buffers. Le gain de 3120 octets n'est **pas** une mesure de gain net du heap.

**Fichiers :** `ModbusTcpServerModule.cpp/.h`, `ModbusTcpSessions.h`, `ModbusTcpSocketServer.h`, `docs/core/psram-ota-memory-relief-2026-11-21.md`, `docs/core/modbus-tcp-psram-buffers-2026-11-22.md`.

## 5 — Priorité donnée à l'OTA en présence de Modbus TCP

Une OTA durablement active (**≥ 300 ms**) provoque la suspension du traitement Modbus TCP et la fermeture contrôlée des sessions/listeners, sans libérer le pool de trames PSRAM. La réouverture n'est annoncée qu'une fois réellement réussie, après fin de l'OTA ou erreur terminale et retour des conditions réseau.

**Choix délibéré :** l'intégrité de l'OTA prime sur la disponibilité Modbus TCP pendant la mise à jour. Les clients peuvent subir des déconnexions ; ils doivent être capables de se reconnecter. Les règles RS485, la résolution de périphériques et les politiques exception/silence ne changent pas.

**Fichiers :** `ModbusTcpServerModule.cpp/.h`, `ModbusTcpServerPolicy.h`, `docs/core/ota-priority-modbus-tcp-2026-11-22.md`.

## 6 — Reboot, checkpoints RTC et dernier échec en NVS

Les étapes OTA sont consignées dans des breadcrumbs RTC sans écrire en NVS par bloc téléchargé. Un marqueur NVS de redémarrage intentionnel, stocké juste avant `ESP.restart()`, permet au prochain boot de contextualiser `[BOOT-RESET]` (l'absence de RTC valide ne démontre pas une coupure d'alimentation).

La cohérence des champs magic/version/checksum de l'enregistrement RTC a été corrigée. Un enregistrement NVS distinct `fw_up_fail` conserve l'opération, l'étape et la cause de la **dernière erreur OTA** même après un succès ultérieur.

**Fichiers :** `src/Core/RebootDiagnostics.cpp/.h`, `FirmwareUpdateFailure.h`, `src/Core/NvsKeys.h`.

## 7 — Diagnostics Web, série et Modbus

- Marqueurs `[OTA-TRACE]`, `[OTA-MEM]`, `[OTA-HEAP]`, `[OTA-STACK]`, `[OTA-NET]`, `[OTA-TCP]`, `[MODBUS-PSRAM]`, `[MODBUS-MEM]`, `[MODBUS-OTA]`, `[MODBUS-RESOLVE]`, `[BOOT-RESET]` et `[OTA-LASTFAIL]`.
- Lignes réparties en événements courts sous la limite LogHub de **159 caractères utiles** : une ligne tronquée ne doit jamais être interprétée comme une mesure de stack/heap complète.
- Compteurs par port 502/503/504 : clients, requêtes, réponses, exceptions, erreurs, latences, suspensions et reprises.
- Échecs de résolution enrichis avec Unit ID, fonction, adresse, device et mode ; pas de fallback de routage RS485 introduit.
- La partie Web restitue les phases OTA téléchargement, SHA, préparation Flash, écriture et commit, avec progression par étape et traductions FR/EN.

**Fichiers :** `LogHub.cpp`, `LogModuleIds.h`, `ModbusTcpServerModule.cpp`, `data/webinterface/app.js`, `sh.html`, `app-core.css`, `i18n/en.json`, `i18n/fr.json`.

## 8 — Wi-Fi / AP et observabilité système

Maintien du correctif évitant qu'un incident STA transitoire ne remplace durablement la configuration Wi-Fi par l'AP de secours. L'instrumentation sysmon évite désormais les lectures de runtime CPU à chaque cycle critique IO ; modes `normal` et `detailed`, capture explicite limitée en fréquence, sans changer les priorités métier.

**Fichiers :** `WifiProvisioningModule.h`, `IOModule.cpp`, `SystemMonitorModule.cpp/.h`, `docs/core/sysmon-safe-diagnostics-2026-11-20.md`.

## 9 — Télémétrie PAC

La représentation JSON du champ `current` est ajustée à deux décimales. Il faut vérifier sur matériel que le chemin MQTT / Home Assistant correspond à l'arrondi attendu.

**Fichier :** `src/Modules/PoolDeviceModule/Drivers/PoolTelemetryJson.h`.

## 10 — Essais réalisés et niveau de confiance

**Essais physiques observés, 22 novembre 2026 :** deux OTA groupées Flow.io terminées sur des slots différents ; images SPIFFS **1 572 864 octets**, firmware **2 837 984 octets**, SHA-256 et commit OK. Le minimum de RAM interne enregistré pendant ces essais était d'environ **31–32 Ko**. Les logs montrent aussi des interruptions TCP et des réponses RTU en retard ; il n'est **pas établi** qu'elles aient été causées par l'OTA.

**Simulation hôte documentée :** 3000 scénarios déterministes de transactions OTA avec trois niveaux d'activité réseau (0, 1, 2 ports) et fautes injectées. Les invariants du modèle passent, mais ce test ne simule pas lwIP, les tâches FreeRTOS, la radio ou la Flash physique.

**Tests ajoutés au dépôt :** `scripts/tests/check_ota_*.mjs`, `check_modbus_*.mjs`, `check_psram_ota_placement.mjs`, `test_wifi_sta_ap_fallback.py`, `simulate_ota_ab_contention.mjs`, tests hôtes Modbus. Leur **présence dans le dépôt n'implique pas qu'ils aient tous été exécutés pour la branche finale**.

## 11 — Validation obligatoire avant fusion

- [ ] Recompiler firmware et SPIFFS avec le profil PlatformIO Waveshare, USB CDC approprié.
- [ ] Exécuter les tests ajoutés et archiver les résultats.
- [ ] Essais A→B et B→A, perte HTTP pendant transfert, Range/206, hash erroné, fallback PSRAM, reboots.
- [ ] Comparer le min heap interne, le plus grand bloc, les marges de piles OTA/Modbus et les latences sous charge équivalente.
- [ ] Modbus : ports 502/503/504, jusqu'à deux clients par port, FC03/FC04, erreurs, reconnexions et reprise après OTA interrompue.
- [ ] FC06/FC16 uniquement sur banc et registres sûrs.
- [ ] Retester STA/AP, Web FR/EN, migration ancienne interface et flux de télémétrie MQTT/HA.
- [ ] Vérifier que les modifications expérimentales sont acceptables en production avant de sortir la PR du mode brouillon.

**Pas de fusion automatique.**
