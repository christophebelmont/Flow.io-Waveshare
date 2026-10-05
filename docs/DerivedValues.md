# Valeurs calculées

Les 5 emplacements `io/value/v00` à `io/value/v04` exposent `name`, `unit`,
`precision`, `enabled`, `expr`, `aggregation` et quatre paramètres numériques
`k0` à `k3`.
`unit` est l'unité affichée de la valeur calculée ; elle est indépendante des
unités des valeurs utilisées dans la formule (ex. `i01.total * k0` peut
s'afficher en `m³`). `precision` (0 à 6) fixe le nombre de décimales utilisées
pour l'affichage et pour l'annonce de changement ; la valeur interne garde sa
précision complète. L'unité et la précision sont propagées au TFT, aux sondes de
l'interface web et à Home Assistant, et prennent effet au redémarrage comme le
reste de la définition.

Les entrées analogiques `io/input/aNN` exposent de même un champ `unit` (unité de
la mesure : `°C`, `mV`, `PSI`…). Les entrées digitales `iNN` n'ont pas d'unité,
y compris en mode compteur : le comptage brut reste en impulsions, et une valeur
convertie avec son unité doit passer par une valeur calculée. Les `iNN` en mode
compteur ne sont ni diffusées sur MQTT ni annoncées à Home Assistant ; seules les
valeurs calculées qui en dérivent le sont.
L’activation, l’expression et le mode d’agrégation prennent effet au redémarrage.
Les paramètres sont appliqués à chaud par la tâche IO, sans recompiler le programme.
Leur copie et les écritures ConfigStore utilisent un mutex commun pour éviter les
lectures partielles de doubles 64 bits sur la cible 32 bits ; aucun verrou n’est
conservé pendant une opération NVS ou un calcul de valeur.
Les valeurs par défaut sont : désactivé, expression `0`, Gauge, paramètres `[1, 0, 0, 0]`.
Ce format utilise ses propres clés NVS ; il ne migre pas les anciennes définitions affines.
La limite actuelle à cinq emplacements ne supprime pas les clés déjà enregistrées
pour V05 à V15. Ces emplacements ne sont plus chargés ni proposés dans la configuration.

## Langage

Les références sont canoniques, en minuscules, avec deux chiffres :

| Référence | Famille / indice |
| --- | --- |
| `a00` … `a31` | Analogique, base 0 |
| `i00.count` … `i15.count` | Compteur brut, base 32 |
| `i00.rate` … `i15.rate` | Impulsions/minute, base 48 |
| `i00.total` … `i15.total` | Total converti, base 64 |
| `i00.flow` … `i15.flow` | Débit converti, base 80 |
| `v00` … `v04` | Valeur calculée, base 96 |

Seules les sources effectivement enregistrées sur la carte peuvent être utilisées.
Les opérateurs sont `+`, `-`, `*`, `/`, le moins unaire et les parenthèses.
Les fonctions sont `min(x,y)`, `max(x,y)`, `abs(x)` et `clamp(x,bas,haut)`.
Les constantes décimales acceptent la notation scientifique ; `k0` … `k3` chargent
les paramètres de l’emplacement courant.

Exemples :

```text
i01.flow - i02.flow
100 * i02.total / i01.total
clamp(a01 - a02, 0, 50)
i01.total * k0 + k1
```

Le hook `validateText` compile le texte proposé avant son enregistrement par
ConfigStore. Un texte invalide est refusé. Au démarrage, chaque expression activée
est compilée une fois ; l’ensemble du graphe est validé avant son installation.
Une source absente/désactivée, un cycle, un mode invalide ou un paramètre non fini
fait échouer la configuration IO.

## Exécution et limites

- Texte : 191 caractères maximum, hors terminateur.
- Programme : 32 instructions, pile de 8 nombres, 16 dépendances distinctes maximum.
- Descente du parseur bornée à 32 niveaux ; aucune récursion dans l’évaluateur.
- Un bloc fixe du registre, prioritairement en PSRAM, contient 37 programmes :
  les 5 expressions et jusqu’à 32 conversions physiques. Les conversions affines
  utilisent cinq instructions du même évaluateur : chargement, paramètre,
  multiplication, paramètre, addition.
- Le tri topologique utilise un espace temporaire au boot, libéré après résolution.
- Aucun texte, allocation ni recherche symbolique dans la propagation numérique.
- Les observations constantes sans dépendance sont renouvelées une fois par seconde
  pour alimenter la couverture de l’historique, sans recompilation.

Le registre complet mesure 58 480 octets dans les tests hôtes ; les tailles exactes
dépendent de l’ABI et sont exposées par `ValueRegistry::storageBytes()`.
Le repli en RAM interne et les erreurs d’allocation suivent la politique du registre.

## Historique et discontinuités

Chaque programme conserve le snapshot précédent de chacune de ses dépendances.
Les différences des compteurs UInt64 sont soustraites en entier avant conversion
vers `double`. L’évaluateur transporte la valeur absolue et sa différence finie
à travers les opérations ; cette différence accompagne le snapshot jusqu’à
ValueHistory, y compris au travers d’autres valeurs calculées.

Pour un produit, la différence est `a_précédent × Δb + b_précédent × Δa + Δa × Δb`.
Il ne s’agit donc ni d’une dérivée ni d’une simple évaluation de l’expression
sur les deltas. Les valeurs publiées et les opérations restent en précision
`double` : les valeurs absolues très grandes, les comparaisons aux bornes et
les calculs non linéaires restent soumis à son arrondi.

En mode Counter, les différences positives sont accumulées. Une diminution,
un changement de génération d’une dépendance ou une recalibration interrompt
la continuité, sans comptabiliser le saut comme une consommation.
Les générations sont suivies séparément pour toutes les dépendances.
Une division par zéro, un résultat non fini ou des bornes `clamp` inversées
produisent une qualité invalide. Une source inconnue/invalide empêche une sortie valide.

## Vérification

```sh
python3 scripts/tests/test_values.py
python3 scripts/tests/test_generate_config_docs.py
node scripts/tests/test_derived_value_tree.cjs
```

Les tests hôtes exécutent le code de production avec ASan et UBSan. Ils couvrent
syntaxe, précédence, arité, limites, dépendances, cycles, validation de texte,
paramètres à chaud, historique et incréments UInt64 au-delà de 2^53 jusqu’à UINT64_MAX.
