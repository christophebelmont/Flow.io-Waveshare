# EventBusModule (`moduleId: eventbus`)

## Rôle

Hôte du bus d'événements interne `EventBus`.
- enregistre le service `eventbus`
- publie `SystemStarted` au démarrage
- exécute `dispatch(...)` en boucle

Type: module actif.

## Dépendances

- `loghub`

## Affinité / cadence

- core: `1`
- task name: `EventBus`
- pile de la tâche: `3584` octets
- priority: `1`
- boucle: `dispatch(16)`, tentative de publication différée, puis 5 ms de délai local et 10 ms de délai commun

## Services exposés

- `eventbus` -> `EventBusService`

## Capacités / limites

- FIFO statique `QueueId::Dispatch` (`1`), `Limits::EventQueueLen` (`40`) entrées
- payload max par événement: `48` octets
- abonnés max: `Limits::EventSubscribersMax` (`60` dans le profil Waveshare)
- ring interne de traces de rejets d'abonnement: `8`

## EventBus

Publication:
- `EventId::SystemStarted` lors des premiers passages de la tâche, avec rejeu jusqu’à l’admission

Abonnements:
- aucun (module hôte du bus)

## Pression et latence

Le Supervisor lit un snapshot cohérent de la FIFO à chaque relevé de santé.
L’occupation et le pic sont mis à jour avec les insertions et les retraits sur
les deux cœurs. Un pic atteint entre deux relevés reste visible. Les compteurs
sont cumulés depuis le démarrage et saturent à leur maximum.

| Trace | Champs |
| --- | --- |
| `EventQueue` | Capacité, occupation, pic, admissions, retraits et refus de `tryPost` avec rejeu. |
| `EventDelay` | ID et producteur de la tête, son âge, et latence maximale admission → retrait pour dispatch avec les ID associés. Les délais sont en microsecondes monotones. |
| `EventDrop` | Pertes sur file pleine, payload trop grand ou payload absent. Les champs ISR et contrôle sont des sous-ensembles des pertes. Les rejets d’abonnement ont leur propre compteur. La ligne passe en `WARN` lorsqu’un compteur de pertes ou de rejets d’abonnement augmente. |

`post` et `postFromISR` abandonnent la publication lorsque la FIFO est pleine.
`tryPost` laisse la responsabilité du rejeu à l’appelant. Le champ `control`
compte les pertes de notifications de configuration, de relais, de mode,
d’alarmes et d’événements planifiés. Après vidage, `accepted` et `dequeued`
doivent coïncider tant que les compteurs sont sous leur maximum.

Les alertes `dispatch slow` et `slow handler` indiquent respectivement un lot
ou un callback lent. Les callbacks s’exécutent hors de la section critique.
`sub reject` donne le motif et l’identifiant des rejets d’abonnement récents.

## Notifications d’état différées

MQTT et HMI mémorisent les clés `DataChanged` dans une boîte à notifications fixe, puis traitent au plus huit clés par passage dans leur propre tâche. Les mises à jour répétées d’une même clé sont regroupées : le consommateur lit le dernier état du DataStore. Une mise à jour concurrente pendant le traitement reste en attente. Les commandes, alarmes et événements de transition ne passent pas par ce regroupement. Aucun traitement de sortie ni parcours de routes MQTT ne se déroule dans le callback `DataChanged` de ces deux modules.

Le DataStore conserve également les clés modifiées pendant toute la durée de fonctionnement. Après le dispatch, la tâche EventBus tente au plus quatre publications par passage. Une file pleine conserve la clé pour réessayer ; aucune notification retenue n’est comptée comme perdue. Les événements ordinaires continuent de signaler leurs pertes éventuelles. Le budget et le parcours circulaire bornent la charge ; les consommateurs lisent le dernier état, sans garantie de recevoir chaque valeur intermédiaire.
