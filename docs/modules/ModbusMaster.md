# Service de transactions RS485

`ModbusMasterService` est le service de transactions de registres consommé par les pilotes d'équipement. Son implémentation est `Rs485TransactionScheduler`, propriété de `IOModule`.

Il prend en charge Modbus RTU et le format explicite Vendor Register RTU. Les paramètres de ligne sont propres à chaque requête ; les émissions sont sérialisées par une tâche unique. Les clients utilisent `submit`, `poll`, `cancelOwner` et `stats`.

Voir [configuration, formats, arbitrage et limites](PoolActuators.md).
