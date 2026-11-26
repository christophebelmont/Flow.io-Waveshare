# Forçage temporaire commun avec MQTT Discovery

Voir [Modes Auto / Manu / Hiver et migration Home Assistant](../../integration/poollogic-operating-modes.md).

Utiliser désormais **actuateurs-forcage-card.yaml**. La carte est repliable, utilise
la couleur normale du thème et devient rouge clair lorsqu'au moins un équipement
est forcé ON ou OFF. Les états d'attente d'heure et d'erreur restent sur fond normal.
Elle utilise `fold-entity-row` et `card-mod`, comme les cartes précédentes.
Aucun script, helper ni capteur à créer manuellement.

Après installation du firmware, laisser Flow.io republier le Discovery, puis
remplacer les anciennes cartes filtration et robot par cette carte commune.
Les douze anciennes entités de forçage sont retirées du Discovery. Le fichier `filtration-dashboard.yaml` contient également cette nouvelle carte
dans la vue Filtration complète. Les anciens fichiers de cartes par rôle et de
package sont des références historiques à ne plus installer.
Les commandes MQTT ciblant explicitement un ancien rôle restent compatibles.

Sept entités sont créées, quel que soit le nombre d'équipements ON/OFF compatibles :

| Entité (préfixe d'exemple `fio`) | Fonction |
|---|---|
| `select.fio_pdm_ovr_target` | Équipement sélectionné |
| `number.fio_pdm_ovr_min` | Durée du prochain forçage, en minutes |
| `button.fio_pdm_ovr_on` | Forcer ON |
| `button.fio_pdm_ovr_off` | Forcer OFF |
| `button.fio_pdm_ovr_release` | Terminer le forçage sélectionné |
| `sensor.fio_pdm_ovr_state` | État et attributs de l'équipement sélectionné |
| `sensor.fio_pdm_ovr_active` | Nombre de forçages actifs et attribut `actuators` |

Adapter `fio` aux identifiants réellement attribués dans Home Assistant, notamment
si des entités ont été renommées. Les lignes d'attribut sont natives : elles
n'ajoutent aucune entité. Le capteur sélectionné expose `actual_on`, `guided_on`,
`control_mode`, `override_value`, `override_remaining_s`, `override_ends_at_utc`,
`override_available`, `slot`, `name` et `id`.

Le capteur global contient les mêmes informations par identifiant stable, par
exemple `state_attr('sensor.fio_pdm_ovr_active', 'actuators')['pd0']['control_mode']`.
Les équipements sans forçage restent présents dans cette liste. Les publications
ont lieu lors des changements et toutes les dix secondes pour le décompte.
L'échéance UTC permet à une carte adaptée de calculer un décompte local.

La sélection est commune à tous les utilisateurs de Home Assistant : un bouton
agit sur la sélection courante du serveur. Changer de sélection n'annule ni ne
modifie un forçage. Pour une automatisation indépendante des utilisateurs,
envoyer une commande MQTT avec `slot` et `duration_s` explicites.
La durée commune est persistante ; la sélection repart sur le premier équipement
compatible après redémarrage. Les forçages en cours gardent leur propre échéance.

Les valeurs MQTT sont indépendantes de la langue : `guided`, `forced_on`,
`forced_off`, `waiting_time`, `persistence_error`. Les cartes doivent tester
`control_mode == 'forced'`, jamais un libellé traduit. Les noms des lignes peuvent
être traduits librement ; les cartes natives affichent les codes MQTT bruts.

La validation finale dans l'instance Home Assistant réelle nécessite le déploiement
et la republication du Discovery.
