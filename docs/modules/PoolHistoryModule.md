# PoolHistoryModule

`PoolHistoryModule` constitue l'historique journalier compact qui servira aux
futurs consommateurs d'analyse, sans dépendre d'un fournisseur d'IA ni de
l'interface web.

## Responsabilités

- conserver les agrégats du jour local courant et du jour précédent ;
- échantillonner toutes les cinq minutes le pH, l'ORP et les températures eau/air ;
- n'enregistrer le pH et l'ORP que lorsque l'état réel de la filtration est connu
  et actif ;
- cumuler chaque seconde le temps réel de filtration et la durée pendant laquelle
  cet état a effectivement pu être observé ;
- basculer les journées selon le fuseau horaire déjà appliqué par `TimeModule` ;
- persister les deux journées dans `ConfigStore` sans bloquer la tâche métier.

Les valeurs sont obtenues par `DomainStatusService` à partir des emplacements
sémantiques de la piscine. Le module ne connaît donc ni identifiant IO configuré,
ni nom de capteur matériel.

## Service public

Le contrat `PoolHistoryService`, enregistré sous `ServiceId::PoolHistory`, ne
propose qu'une opération `getSnapshot`. Elle copie atomiquement :

- la date locale et le début de journée en UTC ;
- les bornes UTC des observations disponibles ;
- les durées de filtration active et observée ;
- pour chaque mesure, le nombre d'échantillons, la première et la dernière
  valeur, le minimum, le maximum et la moyenne.

Le champ `complete` signifie que la journée est close. Il ne garantit pas une
couverture continue : `filtrationObservedSec` et les nombres d'échantillons
permettent au consommateur d'évaluer explicitement cette couverture.

## Persistance

Les clés NVS `phist_today` et `phist_prev` contiennent chacune un enregistrement
binaire de 168 octets. Le format comprend une signature, une version et un
checksum. Une donnée incompatible ou altérée est ignorée.

La journée courante est écrite de manière asynchrone toutes les quinze minutes.
Le changement de jour force l'enregistrement de la journée close et de la
nouvelle journée. Après un redémarrage, seuls le jour courant et son prédécesseur
immédiat sont restaurés ; un enregistrement plus ancien n'est jamais exposé.

Un intervalle de plus de cinq minutes sans exécution n'est pas attribué à la
filtration, car son état pendant cette interruption ne peut pas être établi.

## Implantation mémoire

La structure `PoolHistoryModule::Storage` est construite par placement `new`
dans une zone obtenue avec `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. Elle regroupe
l'accumulateur, les candidats chargés depuis NVS, l'état de suivi runtime et le
buffer de sérialisation. La pile de la tâche `poolhistory` est également allouée
en PSRAM ; les copies de journées utilisées pendant une persistance n'occupent
donc pas la RAM interne.

Il n'existe volontairement aucun repli vers la RAM interne. Si la PSRAM est
absente ou si l'allocation échoue, le service reste enregistré mais
`getSnapshot` renvoie `false` et la tâche n'enregistre aucune donnée. Le mutex,
les pointeurs de services et le pointeur vers la structure restent en RAM
interne : ce sont des éléments de contrôle de taille fixe, et non le stockage
historique susceptible de croître.

Si l'accumulateur adopte ultérieurement des allocations secondaires (par
exemple un tableau dynamique de nombreux jours), celles-ci devront elles aussi
utiliser explicitement `MALLOC_CAP_SPIRAM`. Le fait que l'objet parent soit en
PSRAM ne déplace pas automatiquement les allocations réalisées par ses membres.
