# Données communes du DataStore

Le DataStore et son ValueRegistry existants restent le point de lecture des valeurs.
`src/Core/Values/catalog.json` décrit leurs identités, références de formule et
présentation ; ce catalogue ne stocke aucune mesure supplémentaire.

## Utilisation

- Dans un champ TFT/Web, choisir directement le volume restant ou le remplissage
  du PoolDevice concerné. Aucun IOSlot ni valeur calculée intermédiaire n'est requis.
- Dans une formule, utiliser `pd2.tank.fill_percent`, ou par exemple
  `pd2.tank.remaining_l` pour obtenir des litres. Le temps de marche du jour
  est accessible avec `pd2.uptime_day_s`, en secondes entières.
- Dans la page administrateur **Diffusion des données**, choisir indépendamment **Envoyé dans HA**,
  **Historisé** et **Utilisable dans GUI, Web, TFT et valeurs calculées** pour chaque donnée. Enregistrer puis redémarrer.
- Les références `pdN` désignent des slots stables, indépendants des noms donnés
  aux équipements. Les noms et unités affichés proviennent des métadonnées.

Les entrées analogiques et valeurs calculées conservent par défaut leur publication
Home Assistant. Les données supplémentaires sont proposées sans publication HA
par défaut. La consultation et les formules suivent la troisième capacité, activée par défaut
comme valeur initiale d’une configuration neuve. La désactiver retire la donnée des
choix proposés et rend indisponibles les champs et formules qui la référencent,
sans supprimer leur configuration. La production de la donnée continue ; son
envoi HA et son historique restent indépendants. Une formule utilisant une source
interdite devient invalide même si sa propre publication HA est activée.
La disponibilité est prioritaire sur les trois options : une donnée sans mesure
valide reste visible dans la page Diffusion des données pour sa configuration,
mais est exclue des choix de formules et d’affichage. La page Historique ne liste que
les données à la fois valides (mesure courante disponible) et historisées selon la
politique active : son tableau présente la moyenne journalière sur huit jours, et un
clic ouvre le min/max, la couverture et les ruptures de la journée. Un écart avec la
politique configurée est signalé comme redémarrage requis. Aucune mesure invalide
n’est historisée ; les mesures valides déjà acquises sont conservées et redeviennent
consultables au retour de la donnée.
Une donnée sans producteur actif apparaît indisponible, jamais comme zéro valide.

## Entrées digitales et états MQTT

Les entrées digitales en mode état disposent de références `i00.state` à
`i12.state`, distinctes des compteurs `iNN.count`. Les deux types utilisent
le registre existant, sans copie de mesure : seul le type configuré est produit.
Un état faux est valide et vaut zéro dans une formule. Les noms proviennent
des entrées IO. La publication HA des états digitaux est désactivée par défaut
et se sélectionne dans Diffusion des données, avec les mêmes règles de disponibilité.

Les états digitaux passent uniquement par le catalogue (`rt/value/160` à
`rt/value/172`) ; les anciennes annonces et publications indépendantes
`rt/io/input/iNN` ne sont plus produites. Les annonces historiques déjà retenues
sur le broker ne sont pas nettoyées automatiquement.

Les états MQTT des capteurs ne sont pas retenus. Chaque valeur est republiée
après 120 secondes sans publication réussie, même si sa mesure n’a pas changé.
Tout envoi réussi repousse son échéance propre ; une tentative ou un échec ne
la repousse pas. Les changements restent publiés selon la limitation de débit
habituelle, et la reconnexion republie immédiatement l’état complet.
Le délai commun se règle dans `Limits::Mqtt::Timing::SensorRefreshMs`.
La republication transporte l’état courant, y compris son indisponibilité,
sans modifier l’horodatage d’acquisition ni rendre une mesure périmée valide.
Les sorties et commandes gardent leur politique existante de publication.

## Configuration neuve

La configuration cible part de zéro. Les valeurs calculées sont sélectionnées
uniquement depuis le catalogue des données ; leurs anciens identifiants TFT/Web
ne sont plus proposés ni repris. Les identités HA reposent sur l'identifiant
stable du descripteur, indépendamment du nom de la donnée.
Aucune migration des anciens montages IOSlot → valeur calculée n'est prévue.
Les entrées analogiques utilisent exclusivement leurs bindings physiques primaire
et secondaire. Le sélecteur de source interne n’est plus exposé ni chargé depuis
la configuration persistante.

## Mesures techniques du profil

`Spare`, `Current` et `Voltage` restent les entrées analogiques `a03`, `a06` et
`a07`, accessibles dans le catalogue. Elles n’ont pas de rôle dans le domaine
piscine. Leurs noms, calibrations et bindings par défaut sont définis par
`kAnalogSlotDefaults` dans `WaveshareIoLayout.h`, indépendamment des DomainSlots.
Les identifiants des autres rôles piscine restent inchangés.

## Architecture et disponibilité

Les descripteurs génèrent les options TFT/Web et la table firmware. Les lecteurs
partagent `DataValueView`, qui lit le registre sans cache de mesures. La syntaxe
nommée des formules est résolue vers les mêmes ValueId. Les identifiants runtime
supplémentaires sont `32768 + ValueId`, pour toutes les entrées du catalogue.

Le PoolDevice Manager publie le volume estimé, son pourcentage et le temps de
marche du jour de chacun des 16 équipements dans le registre. Ce dernier suit
le compteur journalier du PoolDevice et sa remise à zéro quotidienne ; les
cumuls hebdomadaires, mensuels et totaux ne sont pas exposés dans le catalogue.
Après cinq secondes sans confirmation, le DataStore invalide ces racines et les
formules dépendantes, même sans module IO actif. L'horodatage de la mesure reste
inchangé ; la transition de qualité porte son propre horodatage pour l'historique.
Les compteurs 64 bits sont formatés sans conversion flottante.

La politique persistante est distincte de la politique active, figée au démarrage.
L'API `/api/data/values` expose les deux et signale le redémarrage requis.
Les valeurs calculées gardent leurs topics existants ; les données supplémentaires
sélectionnées utilisent `rt/value/<ValueId>` et une identité HA stable.

## Découverte Home Assistant

Les libellés des volumes, remplissages et temps de marche reprennent le nom
configuré de la sortie primaire qui pilote l’équipement (par exemple le nom de
`d02`), suivi du nom de la mesure. Ils sont préparés dès le chargement de la
configuration, avant la découverte HA. Sans sortie nommée, le libellé de
l’équipement est utilisé. Les identifiants HA restent stables lors d’un renommage.

Au démarrage, seuls les capteurs du catalogue dont **Envoyé dans HA** est activé
et dont la mesure est valide au moment de l’annonce sont déclarés. Le contrôle a lieu
lors de la construction du message MQTT, après le démarrage des acquisitions,
et non lors du chargement de la configuration. Une donnée indisponible à cet
instant n’est pas annoncée pour ce démarrage. Une entité déjà créée reste dans
HA avec l’état indisponible si sa mesure devient invalide ; aucune suppression
n’est déclenchée par cette indisponibilité.
Une donnée décochée entraîne la suppression de sa découverte
MQTT retenue au redémarrage suivant. Les uptimes journaliers sont désactivés
par défaut, comme les volumes et pourcentages des bidons.

Les sept anciens capteurs liés aux rôles des équipements (deux volumes restants
et cinq temps de marche) ne sont plus déclarés. Aucune suppression automatique
des anciennes entrées HA n’est effectuée ; leur nettoyage reste manuel. Le catalogue est
l'unique voie de déclaration HA de ces mesures. Les boutons de remplissage,
les commandes et les autres entités système restent disponibles.

## Validation

Tests hôte C++ avec sanitizers, tests de génération de configuration et du manifeste,
tests JavaScript de configuration et compilation PlatformIO. La validation TFT,
MQTT/Home Assistant et consommation réelle d'un bidon sur matériel reste nécessaire.
