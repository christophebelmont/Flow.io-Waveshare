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

Après acquittement d'une commande, le pilote série passe en phase `Confirming`
(valeur API 5) et relit prioritairement l'état puis la consigne, ou le retour
unique pour AquaGem. Ces lectures et le contrôle préalable des limites ont la
priorité commande ; elles précèdent la télémétrie sans attendre `pollMs`.
Les arrêts de sécurité, le silence de ligne et l'intervalle minimal propre à
l'équipement restent respectés. Une nouvelle cible annule la confirmation
précédente ; un échec de lecture reste une erreur, sans valeur observée inventée.

Le dialogue affiche immédiatement la consigne demandée avec « Confirmation en
cours », séparément de la dernière lecture. Après relecture, il affiche la valeur
retournée par l'équipement, même si elle diffère de la demande. La qualité et les
seuils de péremption du retour physique restent inchangés. Le firmware et l'image
web SPIFFS doivent être mis à jour ensemble pour bénéficier de cet affichage.

Un équipement en échec ne doit pas monopoliser le bus partagé : après épuisement
des réessais d'une transaction, les nouvelles tentatives de récupération sont
espacées de 1, 2, 4 puis 8 secondes maximum, avec priorité normale. Une réponse
valide réinitialise ce délai. Une nouvelle cible contourne cette attente ; un
nouvel arrêt et ses réessais de transport gardent la priorité sécurité.

La tâche RS485 utilise la priorité 11, au-dessus des callbacks AsyncTCP (10),
pour continuer à traiter les trames pendant les longues requêtes HTTP. Elle
reste non bloquante et cède le processeur à chaque tick. Les seuils de péremption
et l'arrêt de sécurité sur retour périmé ne sont pas modifiés.

`rs485_offline_peer.cpp` teste deux pilotes avec le véritable ordonnanceur :
une PAC silencieuse, une pompe AquaGem fonctionnelle et des pauses applicatives
de 800 ms. Il couvre 41 instants de commande et reproduit la péremption si le
service UART subit lui aussi ces pauses.
