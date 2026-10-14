# Sources analogiques internes : bidons

Chaque entrée `io/input/aNN` peut désormais lire une estimation du PoolDevice
Manager à la place de ses ports physiques. Le choix est effectif au redémarrage.

## Configuration

Dans la configuration de l'entrée analogique :

1. Choisir **Source de la valeur**.
2. Sélectionner le PoolDevice (`pd0` à `pd15`) et **Volume restant estimé (mL)**
   ou **Remplissage estimé (%)**.
3. Donner un nom explicite à l'entrée, régler sa précision, enregistrer et redémarrer.

Les champs de port et de calibration sont masqués pour une source interne. Leurs
valeurs restent conservées pour revenir ensuite au choix **Ports physiques**.
Les unités sont imposées par la source. Aucun filtrage médian ni coefficient
électrique n'est appliqué aux estimations.

Exemple : `io/input/a12/source_value = 115` lit le remplissage estimé de `pd1`.
`114` lit son volume restant en mL. `65535` sélectionne les ports physiques.
L'entrée peut être utilisée par les consommateurs de `readAnalog`, par les
formules (`a12`, par exemple `a12 / 100`), par l'historique et par Home Assistant.
Cette tranche n'ajoute pas de syntaxe de formule pour référencer directement un
PoolDevice : les formules passent par le slot analogique sélectionné.

Les références désignent des slots stables, indépendamment de leur nom ou de
leur fonction par défaut. Choisir un slot sans bidon configuré donne une valeur
indisponible ; le catalogue ne prétend pas que chaque équipement possède un bidon.

## Sémantique et disponibilité

Le PoolDevice Manager reste l'unique propriétaire du volume estimé. Il utilise
son suivi existant du volume injecté et sa commande de remplissage, sans nouvelle
copie persistante de ce calcul. Le pourcentage vaut `100 × restant / capacité`.

- Un bidon vide est une valeur valide : `0 mL` et `0 %`.
- Une capacité non finie, nulle ou négative, un restant incohérent, un équipement
  désactivé, un driver indisponible ou un retour matériel périmé/en erreur rendent
  les valeurs invalides.
- Le producteur confirme ses valeurs chaque seconde, même à l'arrêt, et lors
  d'une publication de métriques, notamment après un remplissage déclaré.
- L'IO lit les sources toutes les 250 ms, indépendamment du polling matériel.
  Après cinq secondes sans confirmation du producteur, la lecture devient
  invalide ; le cycle IO transmet aussi cette invalidité au registre et aux formules.
- L'horodatage de l'entrée reste celui du producteur. Changer la précision ne
  rajeunit pas une ancienne observation.
- Les valeurs sont des **estimations**, pas des mesures physiques. Le snapshot
  MQTT analogique porte `estimated: true` et l'identifiant `source_value`.

## Architecture

`InternalAnalogSources` déclare les racines typées du registre partagé lors de
l'initialisation du DataStore. La plage réservée est `112..143`, avec deux valeurs
par slot : `112 + 2 × slot` pour les mL, puis l'identifiant suivant pour le %.
Les identités antérieures des entrées et valeurs calculées restent inchangées.

`source_value` est validé contre ce catalogue ; aucune entrée analogique ou
formule ne peut être choisie comme source interne. Cette première tranche ne
peut donc pas introduire de cycle entre les valeurs. Le choix actif est figé au
boot et distinct du choix configuré en attente de redémarrage.

Le PoolDevice Manager publie dans le registre ; l'IO lit des snapshots protégés
par son mutex. L'IO ne dépend pas du service PoolDevice ni de ses structures
privées. Le backend `IO_BACKEND_INTERNAL_VALUE` et les statuts de domaine
permettent une entrée active sans port physique.

## Vérifications

- `python3 scripts/tests/test_values.py` : calcul, unités, bidon vide, remplissage,
  changement de capacité, invalidité, fraîcheur, isolation des slots et formules.
- `python3 -m unittest scripts.tests.test_io_boot_binding scripts.tests.test_generate_config_docs` :
  sélection typée, refus d'une source hors catalogue, conservation des ports et génération.
- `node scripts/tests/test_internal_analog_sources_ui.cjs` : catalogue, traductions,
  visibilité conditionnelle et indication de redémarrage.
- Compilation PlatformIO du profil `Flowio-waveshare-esp32-s3`.

La validation sur matériel reste à effectuer avec une pompe configurée, un
remplissage déclaré, puis une consommation réelle.
