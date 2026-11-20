# Diagnostic systeme leger (Wi-Fi / OTA) — 20 novembre 2026

Branche de base : `test/wifi-ping-disable-io-cpu-trace-20261120` (commit `a8bdb4c`), elle-même issue du `dev` de Christophe au commit `c946702`.

## Raison

Sur le `dev` de Christophe, l'instrumentation CPU `traceWakeRuntime` de la tache IO effectuait des lectures repetitives des compteurs FreeRTOS sur le coeur 1. Le test de regression a retrouve des ping Wi-Fi normaux et un OTA operationnel en laissant `spec.traceWakeRuntime = false`. Cette branche preserve cette protection.

## Modes d'observation (sans reactivation des lectures CPU a chaque cycle IO)

- **normal** (defaut au redemarrage) : cycle de rapports sysmon a la periode configuree, avec un plancher de 5 secondes; mesures de pile en rotation, pression memoire, EventBus, TaskTime et TaskWake legers restent actifs.
- **detailed** : memes mesures, rapports synoptiques toutes les 2 secondes. Ne reconfigure pas les priorites des taches, ni AsyncTCP et ne collecte pas les compteurs CPU dans les boucles critiques.
- **capture_once** : via la commande existante `sysmon.capture_tasks`, declenche une capture FreeRTOS ponctuelle executee par `sysmon`. Les compteurs CPU cumulatifs `runtime_counter` sont joints aux lignes `SdkTask` si `configGENERATE_RUN_TIME_STATS` est actif. La commande est limitee a une capture toutes les 10 secondes. Elle ne change pas le mode courant.

Le mode `normal` ou `detailed` se choisit via la nouvelle commande `sysmon.diagnostics_mode`, argument texte **`normal`** ou **`detailed`** (champ `args` de CommandRequest). La reponse confirme le mode. Les changements de mode ne sont pas persistants : ils reviennent a `normal` au redemarrage. Les commandes demandent les memes autorisations que le systeme de commandes Flow.io ; aucune route HTTP non authentifiee n'a ete ajoutee.

Les logs de `sysmon` restent en niveau DEBUG comme sur le `dev` de Christophe. Activer le niveau DEBUG du logger pour les observer.

## Limitation volontaire

Les lignes `TaskWaitCpu` et `TaskWaitTop` issues des deux captures CPU *a chaque attente* ne sont **pas** disponibles en continu. Les restaurer telles quelles reintroduirait la surcharge soupconnee. Les metriques `TaskWake`, `TaskWakeWait`, `TaskTime`, `TaskStack`, `EventQueue` restent observables.

## Tests a faire sur ESP32-S3

1. Compiler la branche et comparer 200 ping Wi-Fi avec le commit `a8bdb4c`.
2. Tester une mise a jour OTA firmware puis SPIFFS avec les deux modes, sans modifier le chemin OTA.
3. Verifier `sysmon.diagnostics_mode` avec `args=normal` puis `args=detailed`, et `sysmon.capture_tasks` deux fois a moins de 10 secondes (le deuxieme appel doit etre refuse).
4. Verifier les journaux `TaskWake`, `TaskStack`, `TaskTime`, `EventQueue`, `SdkTask` en mode DEBUG, avec MQTT et RS485 actifs.

Ce changement n'a pas ete compile ou teste physiquement par l'assistant.
