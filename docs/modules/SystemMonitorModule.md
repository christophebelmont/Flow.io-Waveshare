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

Le Supervisor prélève le high-water mark en octets dans le snapshot
FreeRTOS. Le seuil initial est de 512 octets par tâche, ou 1 536 octets pour
la tâche du module MQTT. Les tâches du SDK apparaissent en `DEBUG` sous leur
identifiant FreeRTOS (`SdkTask`). Un minimum nul déclenche une alerte.

Trois lignes par tâche sont émises au niveau `INFO`, avec trois tâches
par relevé en rotation.

| Trace | Champs |
| --- | --- |
| `TaskSpec` | Identifiant, nom, affinité, priorité, pile configurée, seuil, mode, budget de travail et tolérance au réveil. Un budget à zéro signifie que la durée reste observée sans seuil de dépassement. |
| `TaskHealth` | Génération, état de la tâche, échantillons de pile, minimum libre, passages terminés, dépassement en cours et âge du dernier passage terminé. Une pile non échantillonnée apparaît sous la forme `-`. |
| `TaskTime` | Démarrages tardifs, dépassements du budget, durée maximale de travail, intervalle maximal entre passages et dernière attente programmée. |
| `TaskCapture` | Début et durée de l’appel `uxTaskGetSystemState()` en microsecondes monotones, maximum depuis le démarrage, nombre de captures et nombre de tâches retournées. Cette mesure exclut le traitement et l’impression des relevés. |
| `TaskWakeStats` | Pour IO et RS485, nouveaux retards analysés, nombre dont le retard chevauche une capture et nombre d’échantillons refusés lorsque le tampon PSRAM de 32 entrées est plein. |
| `TaskWake` | Plus grand retard avec chevauchement et plus grand retard sans chevauchement lorsqu’ils existent. `due_us` est l’échéance nominale, `run_us` le retour au début du cycle et `late_us` leur écart, sur la même horloge monotone que `TaskCapture` et `DsPoll`. |
| `TaskWakeWait` | Pour IO, durée observée/nominale pour l’attente propre au module (`module_us`), puis pour l’intervalle entre son retour et le cycle suivant (`loop_us`). |

Le Supervisor compare la fenêtre `[due_us, run_us)` aux deux dernières
captures. Un chevauchement signale une coïncidence temporelle. La mesure
observe l’exécution du cycle et utilise la durée nominale des ticks pour
l’échéance. Elle ne mesure pas l’instant où FreeRTOS rend la tâche prête.

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
réutilise le relevé du Supervisor, sans nouveau parcours des piles FreeRTOS.

## Horodatage du watchdog web

Le watchdog relève `millis()` après lecture du snapshot de santé web, afin qu’une mise à jour concurrente ne produise pas un âge négatif converti en entier non signé. Les âges utilisent une soustraction non signée pour supporter le débordement de `millis()`. L’activité HTTP/WebSocket la plus récente est sélectionnée par son âge, et non par la valeur brute de son horodatage. Les seuils et la politique de redémarrage restent inchangés.
