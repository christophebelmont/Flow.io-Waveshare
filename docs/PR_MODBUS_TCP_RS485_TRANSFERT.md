# Transfert Modbus TCP / RS485 vers `christophebelmont:dev`

> Document de préparation de Pull Request. Aucun merge automatique, aucun changement de la branche amont.

## Références de transfert

- Branche cible : `christophebelmont/Flow.io-Waveshare:dev`.
- Base contrôlée le 19/11/2026 : `9645fc3485d9b047637dc69e234fd0384d2f36b1`.
- Branche source : `stephdust/Flow.io-Waveshare:transfer/modbus-tcp-rs485-to-christophe-dev-2026-11-19`.
- Précédente PR #3 (Web / mémoire / AsyncTCP / OTA) déjà fusionnée dans `dev`.
- Deux nouveaux commits `sysmon` de Christophe conservés par parentage.

## Contenu

1. Trois ports Modbus TCP 502, 503, 504 ; deux sessions simultanées par port (six max), trames MBAP, requêtes séquentielles par connexion, réponses TCP non bloquantes et aucune mise en cache.
2. Toutes les transactions Modbus TCP sont arbitrées par le séquenceur RS485 partagé avec les équipements Flow.io ; modes **équipement existant** et **configuration série manuelle**.
3. Fonctions Modbus standard FC03 et FC04 jusqu'à 125 registres, FC16 jusqu'à 123 registres et FC06 ; conservation de l'Unit ID TCP dans la réponse et adresse RS485 configurée selon le mode.
4. Gestion d'erreurs **exception Modbus** ou **silence**, timeout par port, compteurs de file, latence TCP / attente / RTU, CRC, connexions et refus.
5. Sessions fermées : détection FIN/RST avant admission, keepalive TCP pour disparitions brutales, reprise d'une session inactive sous pression (30 s), sans fermeture arbitraire de clients valides.
6. Configuration Web FR/EN dans Config Store, API authentifiée `GET /api/modbus/ports`, 21 entités HA modifiables et publications MQTT via les services existants. Les changements d'écoute TCP/RS485 prennent effet au redémarrage.
7. Équipements RS485 partagés : application initiale des consignes, adoption de l'afficheur physique après confirmation et protection des priorités de sécurité. AquaGem : délai 60 s et tolérance de 75 tr/min, lecture de vitesse physique. Polytropic : conservation de mode à l'arrêt et vérification des consignes.

## Conflits évités

Branche créée **à partir du commit exact de `dev`** (incluant les merges récents de Christophe), puis nouveau commit ciblé. Les fichiers WebInterface et Bootstrap sont intégrés par ajout de blocs Modbus, sans écraser les modifications `sysmon`.
Les changements antérieurs non Modbus ont été exclus : OTA, Web/AsyncTCP déjà fusionné, scripts de build/version, export de logs, cache d'affichage des équipements, optimisation non liée aux mesures Modbus.

## Validation attendue AVANT fusion

- [ ] Compiler la cible `Flowio-waveshare-esp32-s3` (PlatformIO) et vérifier RAM/pile.
- [ ] Compiler et flasher les ressources Web SPIFFS correspondantes.
- [ ] Exécuter les tests C++ hôtes de MBAP, bridge, sessions, lecture FC03/FC04 et FC16.
- [ ] Lancer `python3 scripts/tests/modbus_tcp_load.py --help` puis un essai FC03/FC04 avec des registres sûrs sur les ports activés.
- [ ] Vérifier deux clients simultanés par port, refus du 3e et libération après FIN/RST, keepalive, éventuel redémarrage client.
- [ ] Comparer les compteurs de `/api/modbus/ports` ; vérifier les réponses d'exception / politique silence.
- [ ] Vérifier Polytropic et AquaGem pendant trafic concurrent et différentes vitesses RS485, sans famine des sécurités.
- [ ] Faire un essai de durée et surveiller mémoire interne, PSRAM, stack, latence TCP/RTU, file d'attente, pertes de communication.

**Statut :** intégré au niveau source et revu par comparaison de commits. La compilation de cette combinaison avec `dev` et l'essai matériel restent à réaliser avant toute fusion.

Références détaillées : [serveur TCP](modules/ModbusTcpServer.md) et [consignes RS485 partagées](modules/SharedRs485Setpoints.md).
