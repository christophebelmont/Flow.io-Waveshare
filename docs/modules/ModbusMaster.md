# Service de transactions RS485

`ModbusMasterService` est le service de transactions de registres consommé par les pilotes d'équipement. Son implémentation est `Rs485TransactionScheduler`, propriété de `IOModule`.

Il prend en charge Modbus RTU et le format explicite Vendor Register RTU. Les paramètres de ligne sont propres à chaque requête ; les émissions sont sérialisées par une tâche unique. Les clients utilisent `submit`, `poll`, `cancelOwner` et `stats`.

Voir [configuration, formats, arbitrage et limites](PoolActuators.md).

Le timeout de réponse ne détruit pas une trame dont des octets ont déjà été
récupérés par le transport : celui-ci attend le silence inter-trame, puis le
codec valide la longueur et le CRC. Cela évite un faux timeout lorsqu'une tâche
retardée vide le tampon UART après l'échéance. Sans octet reçu, le timeout reste
applicable ; une trame incomplète est rejetée au décodage après le silence, et
une réception continue est bornée par la capacité du tampon (256 octets).

Le test hôte `rs485_receive.cpp` couvre cette coordination avec le vrai transport
et un UART simulé, notamment avec une réponse AquaGem à 1200 bauds.
