# Famille 5 — Rapport de simulations Home Assistant / MQTT Discovery

**Date :** 24 novembre 2026  
**Branche :** `stephdust/Flow.io-Waveshare:fix/family5-ha-discovery-psram-20261124`  
**État (mis à jour le 24/11/2026) :** simulations déterministes validées ; **l'utilisateur confirme que la compilation et les essais de famille 5 fonctionnent sur son matériel**, y compris la correction cosmétique du bouton. Revue indépendante et cas de panne complémentaires recommandés à Christophe.

## Résultats

Le modèle d'états basé sur le séquenceur de découverte de `HAModule` et sur les garanties de la file `MQTTQueue` a exécuté **2 384 scénarios** et **369 957 assertions**, **sans échec du modèle**.

Les 2 384 scénarios couvrent :
- De 0 à 240 entités de découverte ; notamment les frontières de bitmap 31/32/33.
- Mesures immédiatement disponibles, seulement après une à quatre passes, ou jamais définies.
- Échecs temporaires et persistants de génération de messages (ex. PSRAM / sérialisation JSON).
- Broker connecté, absent au démarrage, coupé puis reconnecté, ou périodiquement indisponible.
- File MQTT disponible ou contrainte, jusqu'à une admission seulement toutes les 7 occasions.
- Découvertes complètes, partielles après épuisement des quatre passes et cas catalogue vide.

Autres vérifications dans le même jeu de tests :
- **2 049 tailles de bitmaps** (0 à 2 048 entités), séparation des zones `pending` et `deferred`.
- **48 parcours du bouton** de redécouverte (réponse HTTP, activation HA, confirmations et redémarrage).
- **516 cas de débordement des millisecondes** pour la comparaison de délais.
- **15 scénarios ciblés** de capteurs ne devenant disponibles qu'à la quatrième passe, récupérés.
- Dans certains scénarios les délais virtuels atteignent **environ 95 minutes**, lorsque le broker et la file MQTT sont volontairement dégradés. Il ne s'agit **pas** d'une estimation réelle du temps de découverte sur l'ESP32.

### Deux cas limites identifiés et corrigés

1. **Catalogue HA vide :** auparavant, en l'absence de tout message à publier, `published_` restait faux, empêchant la libération de la mémoire one-shot. Le catalogue vide se termine désormais **uniquement après le démarrage et quand MQTT est prêt**, pour ne pas libérer les structures trop tôt.
2. **Reprises épuisées :** une publication impossible après les quatre passes pouvait rester masquée derrière le marqueur de fin de la procédure. Un log `HA discovery INCOMPLETE after retries` annonce maintenant clairement le **nombre d'entités encore manquantes** et recommande un redémarrage/redécouverte.

**Important :** le marqueur global `haAutoconfigPublished` est aussi utilisé par `WaveshareIoAssembly` pour libérer un second tas temporaire de découverte. Lorsqu'on atteint un échec terminal, il doit signaler la **fin du passage** afin d'éviter une fuite de PSRAM ; cela ne signifie pas que toutes les entités ont effectivement été reçues par Home Assistant. Les logs différencient maintenant les deux états.

## Tests ajoutés

```sh
node scripts/tests/simulate_family5_ha_discovery.mjs
python3 -m unittest scripts/tests/test_ha_discovery_retries.py
```

Le script Node.js modélise les états et leurs transitions. Le test Python contrôle l'assemblage du code source. Les deux ne remplacent pas des tests embarqués ni la charge réelle de l'ESP32-S3.

## Vérifications matérielles indispensables

1. Compiler la branche et noter le hash du commit réellement compilé.
2. Démarrer avec MQTT/HA actifs et les capteurs RS485 non joignables : les entités définies doivent apparaître indisponibles après les reprises.
3. Redémarrer puis remettre les capteurs en ligne pendant les différentes passes ; vérifier que les messages de découverte sont bien retenus par MQTT.
4. Déconnecter/reconnecter le broker ; vérifier l'absence de saturation continue de la file et le retour des télémétries.
5. Examiner `HA PSRAM released` et `[waveshare] HA discovery released`, la mémoire interne disponible, les allocations les plus grandes et la PSRAM avant/après.
6. Cliquer sur **Rafraîchir les entités HA** dans ConfigStore : confirmer l'invite de redémarrage après libération du catalogue et l'absence de reboot après annulation.
7. Tester en parallèle une OTA Nextion / firmware + SPIFFS et des requêtes Modbus pour confirmer que l'activité HA ne dégrade pas les priorités.
8. Vérifier dans Home Assistant les identifiants d'entités, états indisponibles, messages `homeassistant/.../config` retenus et l'absence de doublons.

## Limites

Le modèle n'exécute ni `HAModule.cpp`, ni le broker MQTT, ni Home Assistant, ni FreeRTOS. Il ne mesure donc pas les temps réels, la fragmentation effective de la PSRAM, les effets des pertes de packets MQTT QoS 0, ni les boucles watchdog. Les cas dégradés et l'intégration doivent rester sous revue matérielle.

**Préparé pour la revue de Christophe :** l'utilisateur a confirmé le bon fonctionnement. Ne pas assimiler ce retour à l'exécution exhaustive des scénarios de défaillance sur matériel.
