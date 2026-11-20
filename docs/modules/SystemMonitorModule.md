# SystemMonitorModule (`moduleId: sysmon`)

## Rôle

Supervision périodique:
- heap, fragmentation, RSSI/IP WiFi
- santé des tâches FreeRTOS et pression de l’EventBus
- résumé périodique des écritures NVS

Type: module actif.

## Dépendances

- `loghub`
- `eventbus`

## Affinité / cadence

- core: 0
- task: `sysmon`
- attente entre passages de 35 ms, incluant les 10 ms communs
- relevé de santé piloté par `trace_period_ms`, 5 s par défaut, au plus une fois par seconde

## Services consommés

- `wifi` (`WifiService`)
- `config` (`ConfigStoreService`) [référence disponible]
- `loghub`
- `eventbus` (snapshot de la FIFO)

## Services exposés

- `system_health` fournit le dernier relevé agrégé des tâches et de l’EventBus.

## Config / NVS

Module config: `sysmon` (`moduleId = ConfigModuleId::SystemMonitor`, branche locale `1`)
- `trace_period_ms` (`NvsKeys::SystemMonitor::TracePeriodMs`)

## EventBus / DataStore / MQTT

Lecture du snapshot de santé EventBus. Le diagnostic est émis dans le journal.

## Santé des tâches

`ModuleManager` possède le registre à capacité fixe. Les ID combinent l’ID
numérique du module et un rôle stable. Le registre couvre les tâches de
module, RS485, la boucle Arduino et le redémarrage différé. Il conserve
le minimum de pile et les compteurs après la fin normale de Home Assistant.
Le gestionnaire attend que cette tâche se suspende avant d’appeler
`vTaskDeleteWithCaps` depuis la boucle Arduino. Ce chemin libère la pile
PSRAM et le bloc de contrôle alloués au démarrage.

La tâche `sysmon`, sur le cœur 0, relève une pile à la fois toutes les
100 ms environ. Elle parcourt les tâches du registre puis les sources
externes fournies par les modules Web et MQTT. Les échantillons, leur âge
et les durées de lecture sont conservés en PSRAM.

Le seuil initial est de 512 octets par tâche, de 1 536 pour le module MQTT
et de 1 024 pour AsyncTCP. Les identifiants externes utilisent le rôle
`Library` (4), soit 4100 pour `async_tcp` et 5380 pour `mqtt_task`.
Le module MQTT fournit le handle de son client après connexion. Un mutex
protège sa lecture contre l’arrêt du client. Le handle d’AsyncTCP est
conservé après sa création, conformément à la durée de vie de la tâche
dans la bibliothèque installée.

Une réservation de lecture dans `TaskHealth` diffère la suppression d’une
tâche Flow.io jusqu’à la fin du relevé. Le parcours de sa pile s’effectue
en dehors des sections critiques. Le registre conserve le minimum final
relevé par les tâches terminées.

Les rapports sont émis au niveau `INFO`, avec trois tâches du registre
par relevé en rotation et les sources externes à chaque rapport.

| Trace | Champs |
| --- | --- |
| `TaskSpec` | Identifiant, nom, affinité, priorité, pile configurée, seuil, mode, budget de travail et tolérance au réveil. Un budget à zéro signifie que la durée reste observée sans seuil de dépassement. |
| `TaskHealth` | Génération, état de la tâche, échantillons de pile, minimum libre, passages terminés, dépassement en cours et âge du dernier passage terminé. Une pile non échantillonnée apparaît sous la forme `-`. |
| `TaskTime` | Démarrages tardifs, dépassements du budget, durée maximale de travail, intervalle maximal entre passages et dernière attente programmée. |
| `TaskStack` | Identifiant, génération, minimum libre, âge et durée du relevé individuel. |
| `TaskCapture` | Sur demande explicite, début et durée de l’appel `uxTaskGetSystemState()` en microsecondes monotones, maximum depuis le démarrage, nombre de captures et nombre de tâches retournées. Cette mesure exclut le traitement et l’impression des relevés. |
| `TaskWakeStats` | Pour IO et RS485, retards analysés, chevauchements avec une capture, échantillons refusés lorsque le tampon PSRAM de 32 entrées est plein et `max_us` sur tous les retards de l’intervalle. Le pire retard est conservé même après saturation. |
| `TaskWake` | Plus grand retard avec chevauchement et plus grand retard sans chevauchement lorsqu’ils existent. `due_us` est l’échéance nominale, `run_us` le retour au début du cycle et `late_us` leur écart, sur la même horloge monotone que `TaskCapture` et `DsPoll`. |
| `TaskWakeWait` | Pour IO, durée observée/nominale pour l’attente propre au module (`module_us`), puis pour l’intervalle entre son retour et le cycle suivant (`loop_us`). |
| `TaskWait` | Début et fin exacts d’un appel d’attente IO, avec ticks écoulés/demandés. `stage=0` correspond au délai du module, `stage=1` au délai de la boucle commune. |
| `TaskWaitCpu` | Fenêtre de comparaison CPU (`window_us`), temps écoulé dans les relevés (`probe_us`), temps CPU attribué (`accounted_us`) et nombre de tâches retrouvées dans les deux relevés (`matched`). |
| `TaskWaitTop` | Trois contributions CPU les plus élevées pendant l’attente, au format `identifiant:microsecondes`. |

La tâche `sysmon` compare la fenêtre `[due_us, run_us)` à la dernière
capture globale à la demande. Un chevauchement signale une coïncidence temporelle. La mesure
observe l’exécution du cycle et utilise la durée nominale des ticks pour
l’échéance. Elle ne mesure pas l’instant où FreeRTOS rend la tâche prête.

Les compteurs CPU sont prélevés par un lecteur du service `system_health`
juste avant et après chaque attente d’IO. Ce lecteur utilise les tâches
du registre fixées au cœur 1, les sources externes déclarées sur ce cœur
et sa tâche Idle. Les lectures individuelles `vTaskGetInfo()` demandent
uniquement les informations de tâche, avec le parcours de pile désactivé.
La réservation de handle employée pour les piles protège également ces
lectures. Les tableaux de travail et les traces sont alloués en PSRAM.

L’identifiant 4100 désigne AsyncTCP, 512 LogDispatch, 5633 RS485 et 5 Idle
du cœur 1. Les autres tâches Flow.io conservent leur identifiant numérique
habituel. Les compteurs runtime du SDK sont en microsecondes sur ce profil.
Le calcul traite leur débordement et écarte les tâches dont la génération
a changé entre les deux relevés.

La fenêtre CPU comprend l’attente demandée, son éventuel dépassement et
les relevés eux-mêmes. Les compteurs représentent l’attribution CPU de
FreeRTOS, qui peut inclure le temps d’interruptions imputé à la tâche
interrompue. Les tâches SDK hors registre et celles sans affinité fixe
restent hors de cette comptabilisation. Ces mesures donnent les tâches
actives pendant la fenêtre. Une trace d’ordonnancement reste nécessaire
pour connaître précisément l’instant où IO devient prête.

`wait_us` indique la dernière attente prévue par le chemin réellement exécuté.
L’intervalle entre débuts de cycles inclut le travail, cette attente et
l’ordonnancement. La durée de travail exclut le délai volontaire en fin de
cycle. Sur les tâches événementielles, la mesure démarre après réception
d’un élément et la période reste sans objet.

Les budgets initiaux de travail sont 1 ms pour RS485, 25 ms pour l’I/O,
25 ms pour PoolDevice, 50 ms pour PoolLogic, 20 ms pour Alarmes et 25 ms
pour l’EventBus avec le seuil de profilage par défaut. La tolérance de
réveil est de 1 ms pour RS485 et de 2 ms pour les autres tâches périodiques.
Ces valeurs sont des seuils de diagnostic à qualifier sur la carte.

Sur Waveshare, les piles internes de `loopTask`, `mqtt` et `cfg.persist`
réservent respectivement 6 400, 8 192 et 3 584 octets. Cette répartition
rend 256 octets sur les trois réservations de pile avant le coût statique
des chronométrages. Le minimum libre relevé sur `loopTask` avant la
réallocation était de 3 864 octets.

Les alertes de pile et les nouveaux retards des tâches avec budget
apparaissent dans `Task alert`. Le relevé inclut également les agrégats
[EventBus](EventBusModule.md#pression-et-latence), les statistiques de heap
et le résumé des écritures NVS via `ConfigStore::logNvsWriteSummaryIfDue()`.

Le [protocole de qualification](../core/freertos-eventbus-observability-study.md#qualification-sur-carte)
décrit l’essai de saturation et la capture série sur 24 heures.

Dans la page **Informations**, le lien **Détails...** ouvre le résumé du
dernier relevé. L’interface l’actualise toutes les dix secondes tant que
la fenêtre est ouverte et la page visible.
Le serveur fournit ce résumé via `GET /api/system/health`. La lecture Web
lit le résumé de `sysmon`. Le champ `lowest_stack.sample_age_ms` indique
l’âge du relevé de pile, distinct de l’âge du résumé global.

## Horodatage du watchdog web

Le watchdog relève `millis()` après lecture du snapshot de santé web, afin qu’une mise à jour concurrente ne produise pas un âge négatif converti en entier non signé. Les âges utilisent une soustraction non signée pour supporter le débordement de `millis()`. L’activité HTTP/WebSocket la plus récente est sélectionnée par son âge, et non par la valeur brute de son horodatage. Les seuils et la politique de redémarrage restent inchangés.

## Diagnostic global à la demande

La commande `sysmon.capture_tasks` du service de commandes met une demande
en attente et répond `{"ok":true,"queued":true}`. La boucle `sysmon`
exécute ensuite une seule capture `uxTaskGetSystemState()`. Les demandes
reçues avant son exécution sont regroupées. Le tableau SDK est alloué en
PSRAM pour la capture puis libéré. Les traces `TaskCapture` et `SdkTask`
permettent aussi d’observer les tâches absentes du registre Flow.io.
La capture globale peut retarder l’ordonnancement pendant son exécution.

## Mesures mémoire Web

Les rapports `[MEM]`, `[MEM-LOW]` et `[MEM-DROP]` sont produits par
`sysmon`, avec une observation du heap chaque seconde et un résumé toutes
les 15 secondes. Chaque rapport est réparti sur trois lignes de taille
bornée, avec le même horodatage `ms`. Ils utilisent les minima de pile Web et AsyncTCP mémorisés
par l’échantillonneur. Une valeur de pile nulle accompagnée d’une absence
d’échantillon représente une mesure indisponible. La trace MQTT
`client resources` réutilise également le relevé de pile centralisé.
