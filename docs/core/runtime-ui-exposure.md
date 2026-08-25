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

## Génération du manifeste

Le script `scripts/generate_runtimeui_manifest.py`:

- scanne les fichiers `*RuntimeUi.json`
- vérifie l'unicité des `runtimeId`
- génère le manifeste global
- génère un index C++ utilisé par le firmware Waveshare

Sorties:

- `data/webinterface/runtimeui.json`
- `src/Core/Generated/RuntimeUiManifest_Generated.h`

## Lecture batch

L'interface web utilise le contrat de service runtime pour lire des listes d'IDs.

Chemin actuel:

1. le client web appelle `POST /api/runtime/values`
2. le backend découpe la demande en lots bornés
3. le firmware résout chaque `runtimeId` dans `RuntimeUiRegistry`
4. la réponse compacte est convertie en JSON homogène pour le navigateur

## Coexistence avec les anciens snapshots

Les snapshots JSON de type `/api/flow/status*` sont toujours présents pour compatibilité.

Le transport batch Runtime UI existe en parallèle et expose notamment des valeurs pour:

- `system`
- `wifi`
- `mqtt`
- `pool`
