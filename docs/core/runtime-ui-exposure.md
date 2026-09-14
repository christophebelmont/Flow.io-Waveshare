# Exposition Runtime UI

Cette page décrit le mécanisme d'exposition runtime utilisé localement par le firmware Waveshare pour lire des valeurs vivantes sans exporter le `DataStore` brut.

## Vue d'ensemble

Le système repose sur trois éléments:

- des annotations JSON par module (`*RuntimeUi.json`)
- un manifeste global généré pour l'interface web
- un routage binaire compact dans le firmware

Le manifeste est textuel et statique. Les lectures runtime sont numériques et résolues à la demande.

## Identité des valeurs

Chaque valeur exposée possède:

- `moduleId`
- `valueId`
- `runtimeId = moduleId * 100 + valueId`

Helpers utilisés:

- `makeRuntimeUiId(moduleId, valueId)`
- `runtimeUiModuleId(runtimeId)`
- `runtimeUiValueId(runtimeId)`

La clé textuelle (`wifi.ip`, `pool.water_temp`, etc.) est un alias de présentation. L'identité technique utilisée pour le transport est numérique.

## Composants C++

Références principales:

- `src/Core/RuntimeUi.h`
- `src/Core/RuntimeUi.cpp`

Composants exposés:

- `IRuntimeUiValueProvider`
- `IRuntimeUiWriter`
- `RuntimeUiRegistry`
- `RuntimeUiService`

Types transportés actuellement:

- `bool`
- `int32`
- `uint32`
- `float`
- `enum`
- `string`
- `not_found`
- `unavailable`

## Contraintes mémoire

Le chemin runtime ne conserve pas de manifeste JSON en RAM et ne duplique pas le `DataStore`.

Le coût mémoire permanent provient principalement de:

- la table `moduleId -> provider`
- les constantes `constexpr`
- la sérialisation directe dans le buffer de réponse I2C

## Implémentation dans un module

Chaque module qui expose des valeurs Runtime UI:

1. définit ses `valueId`
2. implémente `writeRuntimeUiValue(valueId, writer)`
3. s'enregistre dans `RuntimeUiRegistry`
4. documente ses valeurs dans un fichier `*RuntimeUi.json`

Exemple simplifié:

```cpp
class WifiModule : public Module, public IRuntimeUiValueProvider {
public:
    enum RuntimeUiValueId : uint8_t {
        RuntimeUiReady = 1,
        RuntimeUiIp = 2,
        RuntimeUiRssi = 3,
    };

    ModuleId runtimeUiProviderModuleId() const override { return moduleId(); }

    bool writeRuntimeUiValue(uint8_t valueId, IRuntimeUiWriter& writer) const override {
        const RuntimeUiId runtimeId = makeRuntimeUiId(moduleId(), valueId);
        switch (valueId) {
            case RuntimeUiReady: return writer.writeBool(runtimeId, wifiReady(*dataStore));
            case RuntimeUiIp: return writer.writeString(runtimeId, "192.168.1.10");
            case RuntimeUiRssi: return writer.writeI32(runtimeId, WiFi.RSSI());
            default: return false;
        }
    }
};
```

## Annotations JSON

Les annotations manuelles vivent dans:

- `src/Modules/**/**RuntimeUi.json`

Format attendu:

```json
{
  "moduleId": "wifi",
  "values": [
    {
      "valueId": 1,
      "key": "wifi.ready",
      "label": "WiFi pret",
      "type": "bool",
      "domain": "wifi",
      "group": "Lien",
      "display": "boolean",
      "order": 10
    }
  ]
}
```

Champs utilisés actuellement:

- `moduleId`
- `valueId`
- `key`
- `label`
- `type`
- `domain`
- `group`
- `unit`
- `decimals`
- `order`
- `enum`
- `flags`
- `display`
- `displayConfig`
- `actions` (optionnel)

## Actions déclaratives

Une valeur peut exposer une ou plusieurs actions explicitement autorisées. Le
navigateur transmet uniquement le couple `runtime_id` / `action_id`: le nom de
commande est résolu côté firmware depuis le manifeste généré et ne peut donc
pas être choisi librement par le client.

```json
"actions": [
  {
    "id": "set",
    "command": "poollogic.auto_mode.set",
    "presentation": "switch",
    "input": { "name": "value", "type": "bool" },
    "refreshDomains": ["mode", "equipements"]
  }
]
```

Présentations prises en charge par le schéma:

- `switch`
- `button`

Types d'entrée pris en charge par la route générique:

- aucune entrée (`input` omis)
- `bool`
- `uint32`

L'acquittement d'alarme utilise ce contrat sous la forme d'un bouton appelant
`alarms.reset` avec une entrée `uint32` nommée `id`. Le bouton n'est activé que
lorsque le service d'alarmes déclare l'alarme acquittable. L'identifiant vient
directement du slot retourné par le backend, sans dépendre de sa position ni
effectuer de mapping par libellé dans l'interface.

### Tableau de compteurs dans une fenêtre d'action

Une valeur peut déclarer `displayConfig.actionDialog` pour afficher un bouton
ouvrant un tableau de cibles et de leurs compteurs. `displayConfig.showValue: false` masque la
ligne de valeur tout en conservant ses actions, placées en bas à droite de la carte.
La configuration contient `buttonText`, `title`,
`description`, `inputLabel`, `allLabel`, `metricsLabel`, `rowButtonText`,
`allButtonText`, `confirmationText`, `confirmationHint`, `confirmButtonText`,
`detailsLabel`, `countText` (avec `{count}`) et `successText` (avec `{target}`), ainsi que :

- `optionsUrl` : route locale `/api/runtime/...` retournant
  `{"ok":true,"options":[{"value":0,"label":"Filtration (pd0)"}]}` ;
- `inputAction` : identifiant d'une action bouton déclarée avec une entrée `uint32` ;
- `allAction` : identifiant d'une action bouton déclarée sans entrée.
- `columns` : libellé de chaque période (`label`), chemin de sa durée en secondes
  (`durationKey`, par exemple `running.day_s`) et de son volume en mL
  (`volumeKey`, par exemple `injected.day_ml`).
  `muted: true` peut atténuer une colonne de contexte, comme les totaux.

Les colonnes acceptent également `type: "datetime"` avec `key` (secondes Unix),
ou `type: "enum"` avec `key` et `states` (valeur entière, libellé et ton
`success`, `danger`, `warning` ou `neutral`). `eligibleKey` lie l'activation
des boutons au booléen d'autorisation fourni par le firmware. `automaticKey`
et `automaticText` remplacent le bouton des cibles automatiques par un texte.
`rowPresentation: "text"` affiche un bouton libellé et `destructive: false`
utilise la couleur normale d'action. `emptyText`, `loadingText` et `errorText`
permettent d'adapter les messages au domaine.

Une colonne `type: "switch"` utilise `key` pour l'état booléen réel, `action`
pour la commande, `targetKey` pour l'identifiant de la cible et `eligibleKey`
pour son autorisation de commande. L'action déclare une entrée booléenne et
une cible typée, par exemple `input: {"name":"value","type":"bool"}` et
`target: {"name":"slot","type":"uint32"}`. `POST /api/runtime/action`
valide séparément `input` et `target` avant de construire les arguments du
CommandService selon le manifeste. Le générateur rejette les noms de champs
identiques et les associations de colonnes incompatibles.

Le générateur vérifie les deux associations. Les textes peuvent utiliser les
clés `_t` habituelles. Chaque ligne possède un bouton de remise à zéro, soumis
à confirmation dans une ligne ajoutée juste sous l'équipement (ou en fin de
tableau pour l'action globale), avec Annuler et Confirmer. Aucune commande
n'est envoyée avant confirmation pour ces boutons. Le toggle On/Off envoie
directement sa commande manuelle. Les actions sont bloquées pendant la commande
et la relecture des compteurs. Une ligne actualisée est brièvement surlignée,
en respectant le réglage de réduction des animations. Le tableau compact
défile horizontalement sur mobile et l'explication détaillée est repliable.

PoolDevice utilise ce contrat sur `pool.device_count` dans la carte Équipements.
`GET /api/runtime/pooldevice_options` parcourt tous les slots enregistrés via
`PoolDeviceService.meta`, y compris les équipements désactivés et absents du
tableau de bord. Un échec de lecture retourne une erreur plutôt qu'une liste
partielle. La réponse comprend les durées `running` (`day_s`, `week_s`,
`month_s`, `total_s`) et volumes `injected` (`day_ml`, `week_ml`, `month_ml`,
`total_ml`) lus dans le DataStore. Les métriques indisponibles sont renvoyées
comme `null` et affichées avec un tiret, distinct d'un zéro réel.
`name` et `deviceId` donnent le libellé et l'identifiant séparément, sans
analyse de chaîne côté navigateur. `actualOn` fournit l'état matériel réel
pour l'indicateur de marche, ou `null` si cet état est indisponible.
Le bouton « Gérer les équipements » ouvre également une colonne On/Off liée
à `poollogic.device.write` avec `slot` et `value`. PoolLogic résout le rôle
du slot à partir de sa configuration et appelle la même commande métier que
la tuile correspondante. La filtration utilise `poollogic.filtration.write`,
qui désactive le mode automatique pour les commandes On comme Off ; le robot
conserve son arbitrage manuel. Les slots sans rôle PoolLogic passent par
`pooldevice.write`. Aucune règle métier n'est recopiée dans le popup.
`controllable` désactive le toggle
si l'équipement est désactivé, son état est indisponible ou les écritures
physiques sont suspendues. Les protections sont vérifiées par PoolDevice à
chaque commande. Le toggle conserve l'état confirmé pendant la requête et
en cas de refus ; après succès, une nouvelle lecture fournit l'état affiché.
Les commandes On/Off actualisent aussi le domaine des modes.
Les switches Home Assistant de la découverte Waveshare utilisent également
`poollogic.device.write` avec le slot de leur état runtime et une valeur
booléenne. Le choix de la commande métier est donc partagé avec le popup,
y compris après réaffectation des rôles PoolLogic. Les quatre commandes de
la carte Équipements restent les commandes métier auxquelles ce routage aboutit.
Les actions appellent `pooldevice.uptime.reset` avec le slot fourni
par le firmware, ou `pooldevice.uptime.reset_all` sans entrée. Elles remettent à
zéro les durées et volumes injectés jour/semaine/mois, conservent les totaux et
lèvent le blocage local de durée maximale journalière. L'interface actualise les
équipements, les alarmes et la vue d'ensemble après la commande, puis relit les
compteurs du tableau. Une relecture en erreur efface les anciennes valeurs
pour ne pas afficher des compteurs périmés comme s'ils étaient à jour.

La carte Alarmes utilise la même fenêtre, avec un bouton « Gérer les alarmes »
en bas à droite. Les tuiles sont consultatives. `GET /api/runtime/alarm_options`
liste toutes les alarmes enregistrées via `AlarmService.listIds` et `readState`,
avec leur identifiant, nom, date du dernier déclenchement UTC, condition,
état du latch et autorisation d'acquittement. Les dates sont capturées par le
moteur à l'activation avec une horloge valide, conservées lors de l'acquittement
et remplacées au déclenchement suivant. Elles ne sont pas persistées au reboot.
Une date inconnue est renvoyée comme `null`. Les actions déclarées sont
`alarms.reset` (entrée `id`, sans association par position ou libellé) et
`alarms.reset_all`. Le moteur conserve le contrôle final : une condition active
ou inconnue et une alarme sans latch ne peuvent pas être acquittées manuellement.

## Génération du manifeste

Le script `scripts/generate_runtimeui_manifest.py`:

- scanne les fichiers `*RuntimeUi.json`
- vérifie l'unicité des `runtimeId`
- génère le manifeste global
- génère un index C++ utilisé par le firmware Waveshare

Sorties:

- `src/Core/Generated/RuntimeUiManifest_Generated.h`
- `src/Core/Generated/RuntimeUiManifestJson_Generated.h`
- `src/Core/Generated/RuntimeUiAlarmText_Generated.h`

Le fichier de diagnostic `data/webinterface/runtimeui.json` n'est écrit que si
la variable d'environnement `FLOW_RUNTIMEUI_WRITE_JSON` est activée. Il ne fait
pas partie des sorties normales du build.

## Lecture batch

L'interface web utilise le contrat de service runtime pour lire des listes d'IDs.

Chemin actuel:

1. le client web appelle `POST /api/runtime/values`
2. le backend découpe la demande en lots bornés
3. le firmware résout chaque `runtimeId` dans `RuntimeUiRegistry`
4. la réponse compacte est convertie en JSON homogène pour le navigateur

## Exécution d'une action

Le client appelle `POST /api/runtime/action` avec les paramètres de formulaire:

- `runtime_id`
- `action_id`
- `input` lorsque l'action déclare une entrée

Le backend vérifie l'identité dans `RuntimeUiActionManifestItem`, construit un
argument JSON typé puis délègue l'exécution à `CommandService`. Une action non
déclarée dans le manifeste est refusée avant d'atteindre la registry de
commandes.

## Coexistence avec les anciens snapshots

Les snapshots JSON de type `/api/flow/status*` sont toujours présents pour compatibilité.

Le transport batch Runtime UI existe en parallèle et expose notamment des valeurs pour:

- `system`
- `wifi`
- `mqtt`
- `pool`
