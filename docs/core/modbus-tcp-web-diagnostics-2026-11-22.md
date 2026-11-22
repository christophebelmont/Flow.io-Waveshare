# Flow.io — Diagnostics Modbus TCP et PSRAM dans les logs Web

22 novembre 2026 — branche
`experiment/modbus-tcp-psram-buffers-20261122` (PR #2).

## Problème

Sur la carte de production avec PAC Polytropic et AquaGem raccordées,
le séquenceur RS485 fonctionne et les OTA A/B aboutissent. La tâche
`modbustcp` démarre, mais les métriques Modbus TCP et le succès de
l'allocation PSRAM ne sont pas capturés dans les exports Web : les
diagnostics existants utilisent surtout `Serial.printf`.

Une tâche lancée ne prouve pas qu'un port TCP écoute ni que les buffers
de réception et de réponse sont alloués dans les 8 Mo de PSRAM.

## Ajout de diagnostics

Le module reçoit la catégorie LogHub `modbustcp` (ID stable 35),
exposée dans les niveaux configurables de logs. Les messages passent
dans LogHub / WebSocket / bootlog **en conservant la sortie série**.

Au démarrage des listeners :
`[MODBUS-PSRAM] pool_ready bytes=3120 reused=0 ...`
ou `reused=1` après reconnexion. En cas de défaut PSRAM :
`[MODBUS-PSRAM] alloc_failed ...`, au plus une alerte Web
par minute (le retry interne reste borné à 10 secondes).

Hors OTA, chaque minute, **même si les trois ports sont désactivés** :
- `[MODBUS-PSRAM]` : octets alloués, état ready, configuration
  des ports 502/503/504, statut de station et de listener ;
- `[MODBUS-MEM]` : RAM interne libre/minimale, largest, PSRAM libre,
  watermark de pile `modbustcp` et pression du séquenceur RTU ;
- `[MODBUS-TCP]` : une ligne par port avec clients, pic de clients,
  requêtes, réponses, exceptions, timeouts et rejets ;
- `[MODBUS-ERR]` en présence de compteurs d'erreurs ;
- `[MODBUS-LAT]` si des transactions sont survenues.

Pendant OTA, toutes les 5 secondes :
- `[OTA-NET]` : statut station, listeners, ports, clients, pool PSRAM ;
- `[OTA-NET]` : queue RS485, refus, pic de durée du tick, RAM ;
- `[OTA-TCP]` : trois lignes, une par port, avec requêtes,
  réponses et erreurs ; les compteurs sont cumulés au fil du boot.

Une ligne `[OTA-NET] stage=load_end` signale la fin.
Les anciennes traces longues sur `Serial` restent disponibles.

## Contraintes et tests

- Le plafond LogHub est `LOG_MSG_MAX=160` (159 caractères utiles).
  Les douze formats ont été vérifiés avec des valeurs pessimistes ;
  le plus long fait 133 caractères dans ce calcul.
- Aucune nouvelle allocation dynamique, file de messages, pile ni
  modification des trames Modbus ou des partitions OTA.
- Le volume de logs augmente pendant une OTA : vérifier le compteur
  de messages `dropped` et les latences lorsque plusieurs clients
  TCP sont actifs.
- `ready=0` et `bytes=0` sont **attendus** lorsque les ports
  sont désactivés. Ce n'est pas une panne.
- Les compteurs RTU incluent aussi le trafic de Flow.io : ils ne
  peuvent être attribués à un client TCP individuel.

Tests de source :
`node scripts/tests/check_modbus_tcp_web_diagnostics.mjs`,
`node scripts/tests/check_modbus_tcp_psram_buffers.mjs`,
`node scripts/tests/check_ota_reboot_diags.mjs`.

À tester sur matériel : compilation PlatformIO, deux clients
par port activé, lectures FC03/FC04, reconnexion Wi-Fi, absence
de reboot imprévu et OTA sous charge. N'effectuer les écritures
FC06/FC16 qu'avec des registres sans risque sur équipements de test.

Les protections PAC, la configuration des équipements, les priorités
du séquenceur RS485 et la logique de flash OTA sont inchangées.
