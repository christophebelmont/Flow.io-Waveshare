# Données internes : bidons

Les volumes restants et pourcentages de remplissage sont accessibles directement
via le [catalogue des données](data-catalog.md), pour les affichages, formules,
Home Assistant et historique.

Ils ne se configurent plus comme source d'une entrée analogique : ces entrées
utilisent toujours leurs bindings physiques primaire et secondaire. L'ancien
sélecteur `source_value` n'est ni exposé ni chargé depuis la NVS.

## Sémantique et disponibilité

Le PoolDevice Manager reste propriétaire du calcul du volume estimé. Il publie
les observations dans le ValueRegistry du DataStore. Le pourcentage vaut
`100 × restant / capacité`.

- Un bidon vide est une valeur valide : `0 L` et `0 %`.
- Une capacité invalide, un équipement désactivé ou un retour matériel
  indisponible rendent les valeurs invalides.
- Le producteur confirme ses valeurs chaque seconde, même à l'arrêt, ainsi
  qu'après un remplissage déclaré.
- Après cinq secondes sans confirmation, le DataStore invalide la donnée et
  les formules dépendantes, indépendamment du module IO.
- L'horodatage reste celui du producteur. Les données sont des estimations.

## Identités

`InternalAnalogSources` définit les racines du registre dans la plage `112..143` :
`112 + 2 × slot` pour les litres, puis l'identifiant suivant pour le pourcentage.
Les formules utilisent `pdN.tank.remaining_l` et `pdN.tank.fill_percent`.
Les références désignent des slots stables, indépendamment du nom de l'équipement.

## Vérifications

- Tests hôte des valeurs : unités, remplissage, invalidité et fraîcheur.
- Tests des bindings physiques et de la génération des documents de configuration.
- Test JavaScript : absence du sélecteur et visibilité des deux bindings.
- Compilation du profil `Flowio-waveshare-esp32-s3`.

Le volume restant est exprimé en litres dans l’état de travail, les services,
les commandes (`remaining_l`), le registre et les publications. Les capacités
configurées et les doses injectées restent en mL, avec conversion explicite.
Le format persistant v1 conserve son encodage mL à la frontière de lecture/écriture,
pour éviter de réinterpréter les observations enregistrées avec un facteur 1000.
