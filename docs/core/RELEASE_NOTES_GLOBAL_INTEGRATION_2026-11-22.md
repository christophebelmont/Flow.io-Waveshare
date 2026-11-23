# Flow.io — Notes de version : intégration globale vers `dev`

**Statut :** release candidate / préparation de pull request (non fusionnée)  
**Date :** 22 novembre 2026  
**Périmètre :** évolutions depuis `christophebelmont/Flow.io-Waveshare:dev` (`c946702`)  
**Branche :** `stephdust:experiment/modbus-tcp-psram-buffers-20261122`  
**Version définitive :** à attribuer après revue et validation ; ce document n'annonce pas une nouvelle release publiée.

## Résumé de la version

Cette intégration renforce les mises à jour **OTA A/B firmware + SPIFFS**, réduit certains usages de **RAM interne** grâce à la PSRAM, améliore la reprise HTTP et les diagnostics, et rend explicite la priorité de l'OTA sur les connexions **Modbus TCP**. Elle apporte aussi des améliorations Wi-Fi STA/AP, une interface Web OTA plus détaillée et des outils de test.

## ✨ Nouveautés — Added

### Mise à jour OTA couplée firmware et SPIFFS
- Installation transactionnelle des images firmware et SPIFFS dans les partitions **inactives**.
- Contrôle des tailles, empreintes **SHA-256** et métadonnées du manifeste avant validation du couple.
- Commit final et **redémarrage unique** après validation des deux images ; abandon sans sélection du nouveau couple en cas d'échec.
- Chemin de transition pour les installations utilisant encore les anciens boutons Firmware/SPIFFS.
- Définition d'un manifeste de release versionné et adaptation de la préparation des binaires distribués.

### Téléchargement en PSRAM avant écriture Flash
- Réception de l'image complète en PSRAM, puis vérification SHA-256 **avant** préparation/écriture de la partition inactive correspondante.
- Réserve mémoire de **2 Mio** et contrôle de la taille du plus grand bloc PSRAM contigu.
- Écriture Flash par blocs de **1 024 octets** copiés depuis la PSRAM vers une zone interne.
- Repli explicite sur le flux historique « télécharger et écrire simultanément » si le staging PSRAM n'est pas possible.

### Tolérance aux erreurs HTTP
- Jusqu'à **3 tentatives par ressource**, requête initiale incluse, pour les erreurs réseau transitoires et HTTP 408, 429 ou 5xx.
- Reprise partielle par `Range` avec validation stricte du `206 Content-Range`.
- Redémarrage complet de l'image lorsque le serveur ignore `Range` et répond en `200`.
- Abandon sur erreur permanente, intégrité invalide ou échec Flash, sans tentative de commit.

### Diagnostics persistants des redémarrages et échecs OTA
- Marqueurs RTC pour l'opération et l'étape d'avancement.
- Marqueur NVS ponctuel d'intention de redémarrage (OTA, système, etc.) vérifié au boot.
- Enregistrement NVS indépendant du **dernier échec OTA** (ID, phase, cause, motif), conservé même après une mise à jour réussie.
- Nouvelles traces `[BOOT-RESET]` et `[OTA-LASTFAIL]`.

### Observabilité Modbus TCP
- État du pool PSRAM, compteurs clients et requêtes **par port 502/503/504**, exceptions, erreurs et latences.
- Compteurs de suspensions et reprises Modbus consécutives à une OTA.
- Diagnostics de résolution de périphérique (Unit ID, fonction, adresse, mode, compteur d'échecs) sans modification automatique du routage RS485.
- Exposition des événements à l'interface Web via LogHub en plus de la sortie série.

### Ajustement manuel des compteurs d'impulsions
- Nouveau champ **Valeur du Compteur** sous l'action **Remettre le compteur à zéro** pour les entrées I00 à I15 en mode compteur.
- Chargement **unique** du cumul brut d'impulsions lors de l'ouverture de Config Store (aucun polling en arrière-plan).
- Saisie décimale avec validation stricte `uint64`, confirmation utilisateur et bouton d'enregistrement distinct.
- Modification du compteur dans la tâche d'acquisition IO, en conservant la valeur brute du driver comme référence pour additionner les impulsions futures.
- Sauvegarde immédiate via le checkpoint NVS existant ; nouvelle génération du compteur pour signaler le changement de base.
- Action indépendante du patch de configuration ordinaire ; état et règles de visibilité des autres entrées inchangés.
- La nouvelle valeur brute est publiée sous le **même DataValue** `iXX.count` que le comptage normal, sans doublon, par exemple `i01.count` → `rt/value/33` (topic préfixé par la configuration MQTT de Flow.io).
- Publication MQTT + **Home Assistant Discovery activée par défaut** pour les compteurs bruts `i00.count` à `i12.count` via le catalogue de données (`ha_default=true`). Les réglages d'exposition déjà sauvegardés en NVS sont respectés ; une ancienne désactivation explicite se réactive dans **Données → Exposition et historique** puis par redémarrage.
- Les modifications par l'interface et les impulsions matérielles utilisent `ValueRegistry`, dont la notification `DataChanged` déclenche la publication MQTT/HA ; les valeurs `iXX.total` restent calculées avec C0.
- **À vérifier sur matériel :** compilation firmware/SPIFFS, saisie suivie d'impulsions, redémarrage, échec NVS et stabilité MQTT/HA.

### Débit instantané pour les entrées numériques — Mode 2 (23/11/2026)
- Nouveau choix `2 | Débit instantané` dans Config Store (anglais : `2 | Instantaneous flow`). Aucun changement des modes État et Compteur cumulatif existants.
- Sélection de **quatre unités** : l/min, l/h, m3/min ou m3/h ; C0/K exprime les litres par impulsion, la précision reste configurable.
- Mesure des impulsions par PCNT matériel ; fréquence sur fenêtres d'environ 1 s, débit calculé selon l'unité. Pas de compteur cumulatif défini, exposé ou checkpointé en NVS dans ce mode.
- Présentation directe de `iXX.flow` dans les **listes de données déjà existantes** (TFT et page Web principale), sans ajouter de widget TFT.
- Publications MQTT et Home Assistant via les mêmes ValueIds/topics stables : `iXX.flow` (par exemple `rt/value/81` pour I01). Les unités sont transmises dans les métadonnées ; l'exposition HA est activée par défaut pour ces débits, sauf préférences NVS existantes.
- Paramètres dédiés mode 2 : `Unité` visible ; remise à zéro et modification manuelle du compteur masquées. Changement de mode/unité appliqué après redémarrage.
- **Budget CPU attendu :** maximum deux entrées en mode 2 en production, lecture PCNT dans l'ordonnanceur I/O existant, environ deux calculs de débit par seconde, sans nouvelle tâche et sans écriture NVS périodique.
- Test hôte C++ réussi : `PulseRateOnly` et `PulseFlowUnits` compilés sous `-Wall -Wextra -Werror` avec sanitizers, scénarios deux entrées, quatre unités, arrêt/rebase et compteur de millisecondes. **La compilation firmware ESP32-S3 et les essais CPU/MQTT/TFT/Web sur matériel restent à valider.**
- Documentation détaillée : `docs/core/flow-mode-2-instantaneous-rate-2026-11-23.md`.

## ⚡ Améliorations — Changed

### Gestion mémoire
- Pools JSON de manifestes et d'OTA placés explicitement en PSRAM plutôt que confiés à un allocateur général.
- Buffers Modbus TCP déplacés en PSRAM : **6 RX + 6 TX de 260 octets**, soit **3 120 octets de stockage de trames**.
- Réutilisation des buffers à travers les déconnexions/reconnexions ; refus d'ouverture des listeners lorsque le pool indispensable ne peut pas être alloué.
- Piles internes rehaussées : OTA **8 192 → 10 240 octets** ; Modbus TCP **4 096 → 5 120 octets**. Ces coûts DRAM ne doivent pas être confondus avec des gains mesurés du tas.

### Priorité OTA / charge Modbus
- Pendant une OTA durablement active (**300 ms ou plus**), suspension du traitement Modbus et fermeture contrôlée des connexions/listeners TCP.
- Réouverture des ports après fin ou échec de l'OTA, lorsque les conditions réseau sont réunies, avec confirmation d'ouverture effective.
- Conservation du pool PSRAM pendant la suspension ; pas de modification du séquenceur RS485 ni des règles Modbus exception/silence.
- **Conséquence opérationnelle : les clients Modbus TCP peuvent subir une déconnexion pendant l'OTA.** Leur reconnexion doit être prise en charge.

### Interface Web
- Phases OTA distinctes : téléchargement, vérification SHA-256, préparation Flash, écriture, validation finale, commit et redémarrage.
- Progression dédiée à chaque étape ; différenciation du mode fallback streaming.
- Traductions français/anglais et ajustements HTML/CSS/JavaScript.
- Informations de diagnostic réseau et mémoire consultables dans les journaux Web.

### Surveillance système et Wi-Fi
- Limitation des mesures CPU intensives dans la boucle critique IO.
- Modes sysmon `normal` / `detailed`, capture ponctuelle des tâches limitée en fréquence.
- Maintien de la logique de récupération du Wi-Fi station (STA) sans retour AP inopportun après une perte de liaison transitoire.

### Télémétrie PAC
- Formatage du champ JSON `current` à **deux décimales**. Cohérence MQTT / Home Assistant à vérifier dans la configuration finale.

## 🛠️ Corrections — Fixed

- Correction du checksum/version des breadcrumbs RTC pouvant masquer des étapes OTA lors du reboot.
- Éviter de confondre redémarrage planifié et incident inattendu lorsque les traces RTC sont absentes.
- Préservation de la première cause d'échec OTA avant réinitialisation de l'état d'une opération.
- Découpage des lignes de log trop longues pour respecter les **159 caractères utiles** du LogHub et empêcher les diagnostics de pile tronqués.
- Suppression de certaines traces série redondantes pendant l'OTA pour limiter la pression de journalisation.
- Reprise contrôlée des listeners Modbus après échec terminal OTA au lieu de laisser un état occupé résiduel bloquer la réouverture.
- Réduction du coût de surveillance CPU dans le chemin IO pour protéger la réactivité réseau.

## 🧪 Tests et validations — Verification

**Observations sur matériel (22/11/2026) :**
- Deux opérations OTA groupées ont abouti, sur les deux couples de partitions A/B.
- SPIFFS **1 572 864 octets** et firmware **2 837 984 octets** transférés, SHA-256 et commit validés dans les journaux observés.
- Minimum de RAM interne libre observé : environ **31–32 Kio** durant ces essais ; aucune preuve de panne par manque de RAM pour ces deux cycles.
- Des déconnexions TCP/retards RTU ont également été observés et ne sont pas automatiquement imputables à l'OTA.

**Simulations et tests ajoutés :**
- Modèle de **3 000 scénarios** de transaction OTA avec fautes injectées et variations du nombre de ports actifs ; invariants du modèle respectés.
- Tests statiques et hôtes de politiques HTTP, PSRAM, états OTA, progression Web, logs, reboot, arbitrage Modbus, buffers et récupération Wi-Fi.
- Ces simulations **ne sont pas équivalentes à un test sur matériel** et la présence d'un script ne garantit pas qu'il ait été exécuté sur ce dernier commit.

## ⚠️ Compatibilité et points d'attention — Known limitations

- Les clients Modbus TCP doivent supporter la fermeture temporaire des ports pendant la mise à jour OTA.
- La disponibilité PSRAM conditionne l'ouverture des listeners dans la configuration Modbus avec stockage externe.
- La reprise HTTP ne conserve pas de téléchargement partiel à travers un redémarrage ESP32.
- Les mises à jour ZIP locales et Nextion suivent des chemins distincts du staging PSRAM distant.
- La transition depuis toutes les anciennes versions Firmware/SPIFFS n'est pas réputée universellement validée.
- Contrôler les écritures Modbus FC06/FC16 uniquement sur registres de test sans risque.
- Des essais sous charge prolongée (0 à 3 ports TCP, jusqu'à 2 clients par port), reprise après OTA échouée, Wi-Fi intermittent et comparaison des marges de pile restent requis avant la fusion définitive.

## 📦 Contenu du dépôt

- Firmware et séquenceurs : `src/Modules/Network/FirmwareUpdateModule/`, `src/Modules/Network/ModbusTcpServerModule/`.
- Gestion mémoire, persistance et logs : `src/Core/`.
- Interface Web : `data/webinterface/` et `data/webinterface/i18n/`.
- Compilation/distribution : `platformio.ini`, `scripts/export_binaries.py`, `scripts/prepare_spiffs_data.py`.
- Vérification : `scripts/tests/` et `test/host/`.
- Détails d'implémentation : `docs/core/PR_GLOBAL_INTEGRATION_2026-11-22.md` et les dossiers techniques OTA/Modbus datés des 20–22 novembre 2026.

---

**Statut final :** candidat à la revue et aux tests d'intégration, **pas encore fusionné ni publié comme version stable**.
