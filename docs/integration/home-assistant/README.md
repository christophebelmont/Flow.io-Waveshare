# Dashboard piscine Flow.io — configuration YAML

Cette version utilise huit vues Home Assistant natives et les cartes HACS déjà
utilisées initialement : **Mushroom 5+**, **card-mod**, **mini-graph-card** et
**Modern Circular Gauge**. Aucun fichier JavaScript Flow.io, aucun template
JavaScript, aucun `config-template-card` n'est nécessaire. Les extensions HACS
restent, techniquement, des composants JavaScript existants.

## Installation

1. Vérifier que les quatre extensions ci-dessus sont installées et chargées.
   Modern Circular Gauge doit prendre en charge les templates **Jinja dans
   `min`, `max` et `segments`**.
2. Ouvrir le dashboard piscine : **Modifier → ⋮ → Éditeur de configuration brute**.
3. Remplacer son contenu par **`../home_assistant_dashboard_flowio.yaml`**, entier.
4. Enregistrer et recharger la page.

La ressource `/local/flowio/flowio-pool-card.js?v=1` de la version précédente
n'est plus utilisée. Elle peut être retirée des ressources Home Assistant,
ainsi que son fichier dans `www/flowio`, si aucun autre dashboard ne l'utilise.
Les anciens liens `#flowio=ph` deviennent les onglets natifs (`…/ph`).

## Présentation

- Accueil : état du bassin, indicateurs de connexion, deux jauges côte à côte,
  quatre commandes rapides, modes de filtration et courbes de température/chimie.
- Sept onglets complémentaires : Filtration, pH, Désinfection, Oxygène,
  Équipements, Alarmes et Système.
- Cartes blanches arrondies, séparateurs discrets, mesures sur fonds gris clair,
  accents bleus et courbes colorées inspirés des captures fournies.
- Sur ordinateur : jusqu'à deux sections côte à côte. Sur téléphone : les
  sections s'empilent ; les jauges et commandes restent groupées.
- La navigation utilise la barre d'onglets native Home Assistant, en haut.
  Aucun script ne déplace ou ne masque les éléments de l'application.

Les mesures et graphiques utilisent les données réelles de Home Assistant.
Les 115 identifiants d'entités de la version précédente sont conservés.

## Thème clair facultatif

Le style des cartes est inclus dans le dashboard via `card_mod`. Pour harmoniser
également le fond général et la typographie, copier `flowio-theme.yaml` dans
`/config/themes/flowio-theme.yaml`. Charger les thèmes dans `configuration.yaml`
en fusionnant cette entrée avec la section `frontend` existante :

```yaml
frontend:
  themes: !include_dir_merge_named themes
```

Recharger les thèmes avec l'action `frontend.reload_themes` (ou redémarrer Home
Assistant si le chargement des thèmes vient d'être configuré). Sélectionner
**Flowio Clair** dans le profil utilisateur ou comme thème de chaque vue.
Sans ce thème, le dashboard reste fonctionnel et ses cartes conservent leur style.

## Commandes et états

Les interrupteurs sont natifs. Toucher une valeur de réglage ouvre son dialogue
Home Assistant pour modifier la consigne ou la sélection. Les nombres et les
sélections utilisent `simple-entity` dans les listes pour éviter les grands
curseurs et champs de saisie visibles sur les anciennes captures.

Les boutons de remplissage, recalcul et remise à zéro utilisent les entités
`button.*`. Les acquittements individuels apparaissent sous l'alarme active
lorsque son bouton n'est pas indisponible. Le firmware publie la disponibilité
de ces boutons en fonction de `resettable` et valide chaque commande.

Les états indisponibles ne sont pas convertis en valeurs nulles. Les contacts de
niveau affichent l'état natif Home Assistant sans présumer de leur polarité.
L'absence d'alarme n'est pas présentée comme une garantie de qualité de l'eau.

Les jauges sont centrées sur les consignes, avec des couleurs calculées en Jinja.
Si une consigne est indisponible, les segments sont gris. Ces repères visuels ne
modifient pas les seuils du firmware. Les limites d'origine sont conservées.

Les graphiques comportant une entité `number.*` utilisent `history-graph`, car
mini-graph-card documente les domaines `sensor` et `binary_sensor`. La courbe
pH/ORP de l'accueil utilise deux axes distincts.

## Vérification

Depuis la racine du dépôt, avec Python et PyYAML :

```sh
python3 scripts/tests/test_home_assistant_dashboard_yaml.py
```

Le test contrôle la structure des huit vues, les cartes autorisées, les actions,
les références d'entités et l'absence de JavaScript spécifique. Ce contrôle
statique ne remplace pas le chargement dans votre instance Home Assistant.
Le rendu final dépend des versions des cartes HACS et du thème actif.

Références :

- [Sections Home Assistant](https://www.home-assistant.io/dashboards/sections/)
- [Lignes simple-entity](https://www.home-assistant.io/dashboards/entities/)
- [Mushroom Legacy Template, adaptée aux styles card-mod](https://github.com/piitaya/lovelace-mushroom/blob/main/docs/cards/legacy-template.md)
- [Modern Circular Gauge : templates Jinja](https://github.com/selvalt7/modern-circular-gauge#jinja-templates)
- [Mini Graph Card](https://github.com/kalkih/mini-graph-card)
