# Value / PulseCounter / History — audit et proposition

Date : 24 septembre 2026. Base examinée : `fd9944d`.
Statut : **audit préalable, architecture proposée ; firmware non modifié**.
Décision utilisateur intégrée : aucune reprise des anciens totaux ; compteurs remis à
zéro une seule fois à la migration, puis acquisition et checkpoints en impulsions UInt64.
Décision complémentaire : conserver PcntCounterDriver et son filtrage en l’état, avec
lecture en tâche, sans ajout d’ISR GPIO ou PCNT.
Cette révision porte uniquement sur l'étude, sans implémentation ni remise à zéro réelle.

## 1. Architecture actuelle et composants réutilisables

| Composant | Constat vérifié | Évolution proposée |
| --- | --- | --- |
| `Core/Services/IIO.h` | `IoId` numérique, familles réservées, snapshots bool/float/int32 | Conserver le contrat physique et les consommateurs existants ; associer des ValueId au boot |
| `Core/DataStore` | Modèle runtime généré, setters spécifiques, notification DataKey ; accès direct sans verrou de snapshot | Héberger le registre Value dans cette infrastructure, ajouter des lectures cohérentes par copie |
| `Core/EventBus` | File bornée, payload copié limité à 48 octets, publication non garantie si file pleine | Notification ValueId + séquence ; ne pas utiliser la file comme journal métrologique sans perte |
| `IOModule` | Pools bornés, ordonnanceur, slots Analog/Digital, services numériques | Résoudre et mémoriser indices DataStore/Value au boot ; supprimer les recherches `strcmp` de ces chemins d'acquisition |
| `DigitalSensorEndpoint` | Bool, int32 ou float ; le compteur actif est exposé en float converti | Garder la vue compatible ; exposer count UInt64 et rate séparément dans Value |
| `PcntCounterDriver` | **Driver réellement construit par `allocGpioDriver_`** | Conserver le driver et son filtrage sans modification ; exploiter ses lectures en tâche |
| `GpioCounterDriver` | Driver ISR présent mais non sélectionné ; état alloué en RAM interne | Reste inutilisé ; aucune activation ni modification prévue |
| `ConfigStore` | Preferences/NVS ; blobs runtime dans le même stockage | Réutiliser ce service ; distinguer configuration et checkpoints |
| `ConfigStoreModule` | File asynchrone bornée, mais aucun accusé de commit au producteur | Ajouter un ticket numérique et un état de résultat borné ; ne confirmer un checkpoint qu'après écriture réussie |
| `PoolHistoryModule` | Mesures via DomainStatus, agrégats journaliers piscine, sept jours complets | Généraliser l'accumulateur numérique dans le module existant, garder les règles métier piscine comme adaptateur |
| `PoolHistoryPersistence` | Format versionné, 368 octets/jour, checksum et sérialisation explicite | Réutiliser ces conventions ; ne pas interpréter une ancienne somme de samples comme une somme temporelle |
| MQTT / HA / TFT / PoolLogic | DataKey, RuntimeUi et IOServiceV2 déjà utilisés | Préserver leurs vues actuelles puis ajouter des routes Value numériques |

Chemin actuel : matériel → driver → endpoint → runtime IO/DataChanged → consommateurs.
History lit des slots métier via DomainStatus ; il ne dépend pas directement d'un ADS1115.
Les moyennes actuelles sont `sum / sampleCount`, et le volume de remplissage est estimé
depuis l'activité et un débit configuré. Il n'existe pas encore de chaîne horaire générique.

Bool, float et int32 traversent les trois représentations IOEndpointValue, IoValue et
IOEndpointRuntime. Le count matériel et logiciel utilise int32. UInt64 existe ailleurs
(temps notamment), double dans History, mais aucun contrat IO ne transporte un count UInt64.

## 2. Migration décidée : compteurs à zéro, aucune reprise historique

`IOModule::processDigitalInputDefinition_` ajoute `float(delta) * c0` au total.
`persistCounterTotalIfNeeded_` sauvegarde ce **total converti float** dans `counter_total`.
Le nombre brut n'est pas conservé. Le coefficient est relu pendant le fonctionnement.

Il est impossible de retrouver un count brut historique exact en divisant ce total par
le coefficient actuel : les anciens coefficients sont inconnus, les écritures manuelles
du total sont possibles et la précision float est déjà perdue. Même à coefficient constant,
arrondir le résultat serait une reconstruction approximative, pas une mesure retrouvée.

L'utilisateur a levé ce blocage : **tous les compteurs peuvent repartir à zéro, sans
conservation des anciens totaux ni origine historique**. Le modèle cible est :

- `pulse.count` : nombre exact d'impulsions acceptées, UInt64, source métrologique ;
- `pulse.rate` : impulsions/minute, calculé en tâche ;
- `flow.total = pulse.count × scale`, avec `scale = 1 / pulsesPerLiter` ;
- `flow.rate = pulse.rate × scale` ;
- seules les impulsions brutes sont checkpointées, jamais le total float converti.

Au premier démarrage du nouveau format, écrire et confirmer un checkpoint versionné
initial à zéro, puis démarrer l'acquisition. Le marqueur de migration est porté par ce
même enregistrement : une coupure ne doit pas laisser un marqueur validé sans son count.
Aux démarrages suivants, restaurer le nouveau checkpoint, **sans remise à zéro répétée**.
Un enregistrement présent mais corrompu doit être signalé, pas assimilé silencieusement
à une première migration. Les anciennes clés `counter_total` ne sont jamais relues comme
source du nouveau count ; leur suppression éventuelle est une opération unique de migration.
Ne pas effacer la configuration générale ou reformater NVS pour réinitialiser les compteurs.

Ne pas importer les archives antérieures dans le nouveau modèle History ; ouvrir une
nouvelle couverture temporelle. Le reset initial n'est ni une consommation ni un delta.
Les resets ultérieurs passent par une commande explicite avec changement de génération.
Pour le total dérivé cible, un changement de scale recalcule le total depuis le count brut :
History ouvre alors une nouvelle génération de la valeur dérivée afin de ne pas interpréter
la recalibration comme un volume consommé. Aucun offset historique implicite n'est ajouté.

## 3. Modèle Value proposé

Pas de hiérarchie d'objets par mesure. Une structure typée et un registre borné intégrés
à DataStore : ValueId uint16, union Bool/Int32/UInt32/UInt64/Float ; envisager Float64 pour
les totaux convertis afin de ne pas réintroduire les limites de précision float.
Métadonnées séparées : type, unité numérique, Gauge/Rate/Counter, source numérique,
flags. Runtime : valeur, qualité, instant monotone, séquence, génération de discontinuité.

Table directe ValueId → slot, capacités dans SystemLimits, plages stables pour les
valeurs physiques et les sorties dérivées. Enregistrement et résolution au boot,
rejet des doublons, dépassements, sources absentes et cycles. Tables figées ensuite.
Un pool de transformations affines, triées une fois, calcule les sorties après les sources.
La conversion d'un UInt64 en flottant n'est jamais utilisée pour conserver le count brut.

Nouveaux éléments justifiés : types/registre Value dans DataStore, structure de
transformation affine, accumulateur temporel générique dans History, structure de
snapshot pulse et estimateur de rate testables sans Arduino. Pas de nouveau bus,
pas de service de stockage parallèle, pas de faux AnalogInput.

Les lecteurs copient un snapshot sous synchronisation task courte. Une notification
ne fait que signaler une séquence disponible. History doit recevoir les observations
dans l'ordre via une voie bornée avec détection des pertes, ou accumuler les intervalles
au point de mise à jour ; lire seulement la dernière valeur après plusieurs événements
ne permettrait pas une moyenne temporelle exacte. La perte d'observations doit produire
une couverture incomplète, jamais une moyenne présentée comme complète.

## 4. Acquisition retenue : PCNT existant, sans ajout d'ISR

La décision utilisateur remplace la proposition antérieure de bascule vers GPIO ISR :
**PcntCounterDriver est conservé en l'état**, y compris sa configuration matérielle,
son filtrage logiciel et son mécanisme de repli. Aucun callback GPIO ou PCNT, watchpoint
avec interruption ou traitement ISR supplémentaire n'est prévu dans cette évolution.

La recherche dans src/include/test/lib ne trouve aucune construction ni utilisation de
GpioCounterDriver hors de ses propres fichiers. IOModule inclut PcntCounterDriver,
dimensionne son pool pour cette classe et la construit à la ligne 3532 examinée.
Le driver GPIO ISR reste inutilisé. Son durcissement et les changements de configuration
IRAM précédemment proposés sortent du périmètre retenu.

### Filtrage conservé

Conserver le filtre matériel PCNT et le traitement logiciel par polling existants.
Ne pas ajouter un second filtre dans Value ou dans l'adaptateur IOModule ; les impulsions
acceptées par le driver sont la source unique du count et du rate.
La proposition de temps mort par front en ISR est retirée. Le paramètre existant
counterDebounceUs conserve sa sémantique actuelle ; il n'est pas renommé ou présenté
comme une garantie d'intervalle minimal entre fronts physiques.

### Chemin vers Value et persistance

Matériel → PCNT → lecture du driver en tâche IO → delta entier → cumul logique UInt64
→ Value count/rate → transformations, History et consommateurs.

La tâche IO possède le cumul logique : elle additionne les deltas valides en entier,
puis publie des snapshots cohérents. Les lecteurs ne doivent pas accéder directement
au cumul mutable ; la synchronisation concerne les tâches, sans présumer l'atomicité
UInt64 sur ESP32. Les checkpoints conservent ce cumul d'impulsions et sa génération.
Le total converti en float reste une vue dérivée, jamais la base du comptage.
Un checkpoint ne remet à zéro ni le compteur matériel ni celui du driver.

Rate : `deltaCount × 60000000 / elapsedUs`, calculé en tâche sur la durée réelle de
la fenêtre. Utiliser une fenêtre bornée adaptée à la résolution souhaitée et un timeout
explicite sans nouvelles impulsions observées. Aux très faibles fréquences, le polling
limite la résolution temporelle : ne pas prétendre mesurer la période exacte entre
fronts à partir des timestamps de lecture. Aucun estimateur fondé sur une capture ISR
n'est prévu. Au reboot, ne pas restaurer l'ancien rate comme mesure actuelle.

### Limites du driver inchangé à conserver dans l'étude

Le driver borne son filtre matériel à 1023 cycles APB (environ 12,8 µs à 80 MHz).
Avec le filtrage logiciel activé, plusieurs fronts lus peuvent être ramenés à une seule
impulsion acceptée, et le réarmement dépend de l'état observé par polling. Le repli
actuel à 30000 arrête brièvement PCNT. Ces observations de code restent documentées ;
elles ne motivent plus un changement du driver dans le périmètre décidé.

Le compteur logiciel exposé par le driver reste int32. Un cumul UInt64 dans Value évite
les pertes de précision float mais ne corrige pas un débordement signé interne au driver.
L'étude ne garantit donc pas un comptage continu illimité au-delà de cette limite.
Ne pas inventer une reconstruction par wrap ou réinitialiser périodiquement le driver
pour contourner ce point. Une éventuelle extension de sa capacité nécessiterait un
périmètre distinct ; elle n'est pas incluse dans la présente décision de conservation.
Un recul observé est signalé comme discontinuité, jamais converti en delta unsigned géant.

## 5. Mémoire

Conserver le placement du driver PCNT et de son pool existants. Aucun nouvel état ISR
ou code IRAM dédié au compteur n’est introduit. Privilégier la RAM interne pour le
cumul logique, les snapshots compacts et la synchronisation entre tâches.
PSRAM : archives horaires/journalières, métadonnées volumineuses, buffers secondaires.
Petits snapshots et synchronisation fréquente : privilégier RAM interne.

L'IO possède déjà une allocation PSRAM avec fallback. History alloue actuellement son
Storage exclusivement en PSRAM et demande aussi une pile de tâche PSRAM : le fallback
doit couvrir les deux, pas uniquement ses archives. Un bloc d'archives borné, avec
fallback interne dimensionné explicitement, évite les allocations répétées.

## 6. Persistance et fréquence d'écriture

Les blobs dits runtime passent par Preferences::putBytes : ils vont en NVS, pas dans
la partition personnalisée `runtime`. La partition NVS configurée fait 0x5000 = 20 Kio.
NVS inclut le wear leveling ; cela ne garantit pas une endurance calculable sans connaître
le composant flash, l'occupation et toutes les autres écritures.

Actuellement : sauvegarde après 32 impulsions **ou** 180 secondes, si le total float change.
À 1350 pulses/min acceptées, le seuil de 32 peut approcher 60750 demandes/jour par compteur
(avant effets du polling). À faible activité, le délai peut atteindre 480/jour.
Une requête acceptée dans la file est actuellement considérée persistée avant son commit.

Politique proposée, à implémenter : checkpoint groupé des compteurs modifiés une fois
par heure, paramètres nommés, aucun checkpoint inchangé ; environ 24 commits/jour,
8760/an en activité continue pour un blob groupé, hors changements de configuration,
resets explicites et archives. Pas de seuil de pulses qui contourne la période minimale.
Un résultat de commit doit être retourné ; les échecs déclenchent des retries espacés
et restent visibles, jamais un faux succès. Ne pas bloquer la tâche d'acquisition en NVS.

Persister : configuration rarement modifiée ; count brut UInt64 confirmé, génération et
version du checkpoint (incluant la migration initiale à zéro) ; archives
fermées selon une capacité qui tient réellement dans le stockage. Ne pas persister rate,
timestamps pulse, min/max actifs, sommes intermédiaires, origine historique ni total
dérivé recalculable. La sauvegarde périodique de `counter_total` float doit disparaître.

Le stockage des archives horaires complètes ne peut pas être ajouté aveuglément dans
20 Kio partagés. Dimensionner d'abord la rétention et mesurer l'occupation ; conserver
en RAM les fenêtres volatiles et les archives journalières compactes existantes tant que
le budget durable n'est pas établi. Ne pas changer la table de partitions implicitement.

## 7. Coupure, reboot, reset et History

Perte à la coupure = impulsions reçues depuis le **dernier commit réussi**, plus les
éventuelles pertes physiques d'acquisition. Avec un commit horaire réussi et une latence
de service bornée L, la fenêtre est au plus 3600 s + L. À 22,5 pulses/s et 450 pulses/L,
une heure représente 81000 pulses, soit 180 L. Sans borne sur la fréquence et la latence,
ou en cas de panne NVS persistante, aucune limite absolue en pulses ne peut être promise.

Reboot : restaurer seulement count et génération ; rate inconnu jusqu'à acquisition,
puis zéro selon timeout. History ne relie pas aveuglément son dernier sample d'avant
reboot au count restauré. Reset, recul ou changement de génération ouvrent un segment
de couverture ; aucune soustraction unsigned négative et aucun wrap géant inventé.

Gauge/Rate : min/max, somme valeur × durée couverte, durée couverte. Scinder aux limites
horaires/journalières et exclure périodes invalides. Jour = sommes et durées horaires
additionnées, jamais moyenne non pondérée des moyennes. Distinguer durée monotone et
calendrier local, notamment changement d'heure et correction de l'horloge.
Counter : soustraction entière avant conversion, somme des deltas des segments valides ;
la somme peut différer de end-start en présence de resets, qui doivent être signalés.
Une différence de compteur entre deux observations chevauchant une limite horaire ne
permet pas de connaître sa répartition exacte : prévoir une capture à la limite ou marquer
l'estimation/couverture, sans inventer une précision temporelle.

## 8. Dépendances et ordre de migration

1. Encoder le checkpoint UInt64 versionné et la migration unique à zéro décidée ci-dessus.
2. Introduire Value dans DataStore et son service ; conserver les vues IO existantes.
3. Raccorder les lectures du driver PCNT inchangé ; tester les deltas, snapshots UInt64,
   le rate par fenêtre et les commits acquittés.
4. Raccorder Analog, Digital State et Pulse count/rate ; résoudre les indices au boot.
5. Ajouter les transformations et la configuration numérique bornée.
6. Généraliser les accumulateurs de History en conservant les conditions métier piscine.
7. Ajouter diagnostics et routes consommateurs, puis valider la carte sous charge.

Dépendances : IO produit dans DataStore ; transformations lisent/écrivent le registre ;
History consomme Value et Time ; ConfigStore possède l'écriture durable ; consommateurs
lisent les services. Aucune dépendance de DataStore vers IO ou History.

## 9. Validation attendue et état réel

Tests à ajouter : 60 pulses/60 s, fenêtres irrégulières, fréquences faible/élevée, arrêt,
reprise, rebonds, timestamps zéro/wrap/inactivité longue, count au-delà de 32 et 53 bits,
concurrence entre tâches sur les snapshots, restauration, reset, saturation et erreurs
de capacité. Les tests du cumul UInt64 au-delà de 32 bits utilisent des deltas simulés ;
ils ne prouvent pas que le driver int32 inchangé supporte cette durée de fonctionnement.
Conversions : 1350/450 = 3 L/min ; 154800/450 = 344 L.
History : pondération irrégulière, frontière heure/jour/DST, invalidité, reset, recul,
perte d'observations. Checkpoint : commit, nouvelles pulses, coupure, restauration ;
file pleine, échec d'écriture et commit reçu pendant l'acquisition.
Migration : ancien total float non nul ignoré, checkpoint initial zéro confirmé avant
acquisition, coupure pendant migration, reboot après nouvelles impulsions sans second
reset, checkpoint corrompu signalé, aucune reprise d'ancienne archive.
Filtre : vérifier que le raccordement Value ne change ni les paramètres ni le comportement
du filtrage PCNT existant ; aucun test ne doit supposer le nouveau temps mort ISR retiré.

Sur carte : générateur d'impulsions et référence indépendante pendant NVS, réseau et
activité HMI ; caractérisation de la cadence de polling et du rate par fenêtre, sans
modifier le driver. Les diagnostics accepted/rejected reprennent uniquement les
statistiques réellement fournies par le driver, avec leur sémantique existante.
Diagnostics attendus : accepted/rejected, rate, count logique, dernier commit confirmé,
nombre d'écritures réussies/échouées, slots Value/History utilisés, octets RAM/PSRAM.

**Résultats de cette étape :** lecture du code et de la configuration locale uniquement.
Pas d'implémentation, pas de compilation ni de tests fonctionnels prétendus réussis.
Consommation avant/après non mesurée ; aucun changement de consommation du firmware.
Seul fichier créé : `docs/ValueArchitectureAudit.md`.
Révision après décision utilisateur : seul ce document a été modifié ; aucun compteur
réel n'a été remis à zéro et aucun fichier de code n'a été changé.

## Références externes vérifiées

- [NVS, ESP-IDF 5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/storage/nvs_flash.html) : flash, journalisation, wear leveling et coupure.
- [PCNT, ESP-IDF 5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/pcnt.html) : accumulateur, filtre et option ISR IRAM.
- [Concurrence flash et ISR](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html) : drapeau IRAM et dépendances code/données internes.
