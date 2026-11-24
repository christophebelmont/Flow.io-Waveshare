# PoolLogic : Auto, Manu et Hivers

PoolLogic reste toujours actif. Un seul sélecteur **Mode de fonctionnement**
détermine les automatismes autorisés. Les protections pression, dépendances
entre équipements, durées maximales et défauts matériels restent appliquées
dans les trois modes.

Le réglage se trouve dans **Piscine → Pilotage général**, dans ConfigStore sous
`poollogic/modes/operating_mode` et dans le sélecteur Home Assistant
**Pool Operating Mode**. Le tableau de bord utilise la même valeur.

## Choisir un mode

| Fonction automatique | Auto | Manu | Hivers |
|---|---|---|---|
| Filtration selon la plage calculée | Oui | Non | Oui |
| Démarrage et maintien antigel | Non | Non | Oui |
| Correction pH | Selon le type de correction | Non | Non |
| Chlore/brome, électrolyse ou oxygène actif | Selon le traitement choisi | Non | Non |
| Chauffage assisté | Si configuré | Non | Non |
| Robot quotidien | Oui | Non | Non |
| Remplissage selon le niveau | Oui | Non | Non |
| Protections des équipements | Actives | Actives | Actives |

### Auto

La filtration suit sa plage calculée à partir de la température d'eau et des
contraintes horaires. Le chauffage assisté et une dose O₂ en attente peuvent
prolonger ou demander la circulation. Les dosages attendent les délais de
filtration et leurs conditions de fonctionnement habituelles.

### Manu

Aucune fonction automatique ne démarre la filtration ni ne régule un équipement.
Les commandes directes des équipements sont disponibles. Passer en Manu arrête
les demandes de dosage, chauffage, robot et remplissage automatiques.

**La filtration déjà demandée en marche conserve sa demande manuelle.** Manu
n'est donc pas un arrêt général. Pour arrêter la filtration, envoyer ensuite
une commande d'arrêt. La sécurité pression peut toujours la couper.

### Hivers

La filtration suit sa plage calculée et peut démarrer en plus lorsque la
température extérieure passe sous `winter_start_t`. Lorsqu'elle tourne déjà,
le maintien antigel la garde en marche tant que la température extérieure est
inférieure ou égale à `freeze_hold_t`. Les valeurs par défaut sont respectivement
−2 °C et 2 °C ; elles restent configurables.

Les dosages pH et désinfection, l'électrolyse, le protocole O₂, le chauffage,
le robot et le remplissage automatiques sont suspendus. La filtration n'est
pas imposée 24 h/24. Hivers est un choix explicite de l'utilisateur ; le système
ne change pas de mode à sa place selon la saison.

Sans mesure extérieure disponible, l'antigel ne peut pas déclencher une demande
sur température ; la filtration programmée continue de s'appliquer.

## Désactiver uniquement la correction pH

Le sélecteur **Correction pH**, dans `poollogic/ph/correction_type`, propose :

| Choix | Résultat en Auto |
|---|---|
| Désactivée | Aucun dosage pH ; filtration et désinfection continuent |
| pH− | Injection d'un produit abaissant le pH selon la consigne |
| pH+ | Injection d'un produit augmentant le pH selon la consigne |

Le produit installé doit correspondre au sens choisi. Les consignes et paramètres
PID restent mémorisés quand la correction est désactivée. En Manu ou Hivers,
le type de correction reste enregistré mais la régulation est suspendue.

## Désactiver uniquement la désinfection

Choisir **Désactivé** dans **Type de désinfection**
(`poollogic/modes/disinfection_type`). Ce choix bloque toutes les stratégies
automatiques de désinfection : pompe chlore/brome, électrolyse et oxygène actif.
La filtration et la correction pH peuvent continuer en Auto.

Les trois autres choix sont Chlore/Brome, Électrolyse et Oxygène actif. Il n'existe
plus d'interrupteur « Traitement auto » indépendant.

## Commandes manuelles et forçages temporaires

- En Manu, les commandes directes des équipements pilotés par PoolLogic sont
  acceptées, sous réserve des protections. Elles ne modifient ni le mode ni
  les types de produits configurés.
- En Auto et Hivers, utiliser **Forcer…** pour une intervention temporaire sur
  un équipement compatible. Les commandes directes sont refusées ; les réglages
  de consigne restent modifiables.
- Un forçage est une demande manuelle explicite, y compris en Hivers ; il ne
  réactive pas la régulation. Les protections restent prioritaires.
- À l'expiration, la demande guidée courante reprend. En Hivers, les demandes
  guidées des régulations suspendues sont à l'arrêt.
- Les équipements sans forçage compatible (notamment RS485) nécessitent Manu
  pour une commande directe marche/arrêt.
- Changer de mode termine les forçages des rôles gérés par PoolLogic. Modifier
  la correction pH termine celui de la pompe pH ; modifier le traitement termine
  ceux de la pompe de désinfection et de l'électrolyseur.
- Un simple redémarrage conserve les forçages compatibles et leur échéance,
  selon les règles de [persistance des forçages](../modules/TimedActuatorOverrides.md).

L'éclairage et les équipements sans rôle PoolLogic conservent leurs commandes
habituelles. Les protections PoolDevice leur restent applicables.

## Changements de mode et reprise

Les sorties automatiques concernées reçoivent explicitement une demande d'arrêt
lors d'un changement de stratégie. Les régulateurs pH/ORP sont réinitialisés.
Au retour en Auto, les conditions et délais de circulation sont vérifiés avant
le dosage. Une dose O₂ inachevée reste mémorisée ; le temps passé en Manu ou
Hivers n'est pas compté comme du volume injecté.

Les commandes directes restent bloquées pendant une transition inachevée, par
exemple si la sauvegarde de la fin d'un forçage échoue. PoolLogic réessaie avant
de rendre la main. La mise en service des sorties au démarrage attend la
migration et la reconstruction des demandes selon le mode restauré.

## Contrat ConfigStore et MQTT

Les valeurs sont numériques et validées avant modification :

| Champ | Valeurs |
|---|---|
| `poollogic/modes/operating_mode` | `0` = Manu, `1` = Auto, `2` = Hivers |
| `poollogic/ph/correction_type` | `0` = Désactivée, `1` = pH−, `2` = pH+ |
| `poollogic/modes/disinfection_type` | `0` = Chlore/Brome, `1` = Électrolyse, `2` = O₂, `3` = Désactivé |

Exemple de patch ConfigStore :

```json
{"poollogic/modes":{"operating_mode":1},"poollogic/ph":{"correction_type":1}}
```

Commande sur `<base>/<device>/cmd` :

```json
{"cmd":"poollogic.operating_mode.set","args":{"value":2}}
```

Le mode est publié dans `cfg/poollogic/modes` et dans l'agrégat `cfg/poollogic`.
Le runtime `2401` est désormais une énumération `pool.operating_mode`.
Les anciens runtimes `2402`, `2403`, `2404` et les anciennes commandes de modes
booléens sont retirés. Les bits d'état des écrans historiques sont des
projections en lecture seule du mode et des autorisations calculées.

## Migration des installations existantes

| Configuration précédente | Nouveau mode |
|---|---|
| PoolLogic désactivé | Manu |
| PoolLogic actif, Auto désactivé | Manu |
| Auto actif, Hiver désactivé | Auto |
| Auto actif, Hiver activé | Hivers |

L'ancienne désactivation pH devient Correction pH désactivée. Sinon, le sens
pH−/pH+ est conservé. L'ancien traitement auto désactivé devient Type de
désinfection désactivé seulement pour Chlore/Brome : ce drapeau ne pilotait
pas l'électrolyse ni l'O₂.

Les nouvelles valeurs déjà enregistrées sont prioritaires. Les anciennes clés
NVS sont supprimées seulement après sauvegarde des remplacements et du marqueur
de migration. Un échec de sauvegarde bloque la reprise automatique des sorties
et entraîne une nouvelle tentative. Une installation sans ancienne configuration
démarre en Manu avec correction pH et désinfection désactivées.

Les champs `enabled`, `auto_mode`, `winter_mode`, `ph_auto_mode`, `dis_auto_mode`
et `ph_dose_plus` ne sont plus exposés par PoolLogic. Les anciennes intégrations
qui écrivent ces champs doivent utiliser le nouveau contrat.

## Home Assistant et écrans locaux

Deux nouveaux sélecteurs sont annoncés : `pl_operating_mode` et
`pl_ph_correction` (par exemple `select.fio_pl_operating_mode` et
`select.fio_pl_ph_correction` avec le préfixe `fio`). Adapter les noms aux
identifiants réellement attribués dans Home Assistant.

**Aucun effacement des anciennes entités Home Assistant n'est ajouté ou lancé
par cette migration.** Leur suppression et l'adaptation des automatisations
existantes sont manuelles. Les exemples YAML du dépôt utilisent les nouveaux
sélecteurs ; ils ne modifient pas une instance Home Assistant.

Le TFT local lit le même mode. Le menu de configuration Nextion expose le
sélecteur à trois choix. Dans le protocole Home historique, l'action `0x02`
accepte `0`, `1`, `2`, l'action `0x0A` fait défiler Manu → Auto → Hivers,
et `0x0D` sélectionne Hivers. Les anciennes actions pH/ORP auto `0x0B` et
`0x0C` sont retirées.

Les projets `.HMI` et les images `.tft` Nextion existants ne sont pas recompilés
par PlatformIO. Leur présentation Home historique doit être mise à jour dans
Nextion Editor pour retirer visuellement les anciennes touches et présenter
les trois choix. Le menu de configuration dynamique utilise déjà les nouveaux
champs ; les anciennes touches pH/ORP ne peuvent plus modifier un automatisme.

## Documentation associée

- [Logique métier piscine Waveshare](flowio-poollogic-business.md)
- [Référence PoolLogicModule](../modules/PoolLogicModule.md)
- [Forçages temporaires](../modules/TimedActuatorOverrides.md)
- [Dashboard Home Assistant](home-assistant/README.md)
- [Protocole Nextion](nextion-esp-protocol.md)
