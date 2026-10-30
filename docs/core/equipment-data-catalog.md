# Catalogue des mesures des équipements

Les pilotes série déclarent leurs mesures avant la résolution des formules, dans
`Module::onConfigPrepared`. Le catalogue décrit les possibilités en Flash ; seules
les mesures prises en charge par un équipement configuré et activé sont déclarées
au démarrage. Les mesures absentes ne sont pas proposées dans « Diffusion des
données ». Une mesure déclarée mais périmée reste visible et configurable.

## Identités et stockage

`src/Core/Values/equipment_metrics.json` est la source des identités de mesures.
Le générateur produit les identifiants typés `EquipmentMetric` et les descriptions
utilisées par les références de formules, l'interface et MQTT. Ne pas renuméroter
une métrique existante : son identité est `176 + emplacement × 32 + métrique`.
Les identifiants antérieurs à 176 restent inchangés. Renommer un équipement ne
change pas ses identités. Remplacer son profil réutilise les mêmes identités pour
les mesures de même nature ; les réglages persistés des mesures devenues absentes
restent en NVS pour une réactivation ultérieure.

Le registre possède un index de pointeurs en PSRAM et alloue un enregistrement
uniquement lors de la déclaration d'une valeur. La lecture, l'écriture et la
propagation des formules n'allouent pas de mémoire. Les tables de configuration
croissent par blocs en PSRAM. Les paramètres d'exposition supplémentaires sont
enregistrés uniquement pour les mesures déclarées. Chaque équipement a une branche
de notification ; les clés NVS restent propres à chaque mesure.

L'historique traduit les identifiants stables en indices de séries compactes et
alloue au démarrage uniquement les séries définies et historisées. Les routes MQTT
sont dimensionnées selon la sélection effective ; leurs indices sont sur 16 bits.
Les tables de découverte Home Assistant croissent en PSRAM. Les libellés sont
alloués uniquement pour les entités sélectionnées et libérés après la découverte
unique. Les descriptions possibles restent en Flash. La suppression des anciennes
entités HA couvre aussi les mesures devenues absentes.

Ces structures exigent la PSRAM : un échec d'allocation est signalé, sans reporter
leur volume sur la RAM interne. Cette politique concerne la configuration, le
registre, l'historique des valeurs et les nouvelles tables de diffusion sur la
carte Waveshare ESP32-S3.

## Mesures et qualité

Pour chaque équipement RS485 :

- `pdNN.requested` : consigne demandée, dans l'unité configurée du pilote ;
- `pdNN.observed` : valeur relue, si le pilote dispose d'un retour ;
- `pdNN.running` : marche confirmée, si le pilote dispose d'un retour ;
- les champs du profil de télémétrie Polytropic, s'il est activé, notamment
  `pdNN.water_in` et `pdNN.water_out`.

« Valeur relue » ne signifie pas nécessairement vitesse mécanique mesurée : cela
dépend du registre et du protocole de l'équipement. Aucune mesure n'est reconstruite
à partir de la consigne demandée. Les booléens sont utilisables dans les formules
avec leur valeur numérique 0 ou 1.

Les dates de lecture viennent du pilote et sont converties vers l'horloge monotone
64 bits, y compris au débordement de `millis()`. Les blocs de télémétrie gardent
leur validité indépendante. Une péremption invalide les formules dépendantes ; elle
ne produit pas un zéro valide. Il n'y a ni nouvelle tâche ni lecture Modbus
supplémentaire pour la diffusion.

## Sélection et redémarrage

Les nouvelles mesures sont utilisables par défaut, avec Home Assistant et
l'historisation désactivés. Les trois réglages sont indépendants. Comme pour les
valeurs existantes, les changements de diffusion prennent effet après redémarrage.
Un changement de profil ou l'activation d'un équipement reconstruit également son
catalogue au prochain démarrage. Les commandes HA existantes restent séparées des
capteurs de lecture.

Exemple de formule : `pd02.water_out - pd02.water_in`.

## Vérification et mémoire

`python3 scripts/tests/test_values.py` couvre le stockage parcimonieux, les
identifiants supérieurs à 255, les formules, la péremption/récupération, les registres
signés et l'historique compact. Les tests de catalogue, découverte HA, configuration,
pilotes et interface complètent ces contrôles.

Les tailles exactes dépendent de l'ABI cible. Les logs de démarrage `Serial catalog`
affichent le nombre de mesures, leur stockage en PSRAM, la variation de RAM interne
et son plus grand bloc libre. `Selected value history` indique le nombre de séries
et les octets réservés. Les chiffres de compilation ne remplacent pas une mesure
sur carte pendant la découverte HA et les publications.
