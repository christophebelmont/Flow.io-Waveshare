# Serveur Modbus TCP de Flow.io — configuration et validation

## Configuration Web, MQTT et Home Assistant

Dans la page **Configuration / Config Store**, ouvrir **Network → Modbus TCP**
puis l'un des ports **TCP 502, TCP 503, TCP 504**. La sélection
**Paramètres RS485** remplace l'ancien bouton d'activation :
- **Aucun** : port désactivé, autres champs masqués.
- **Équipement Flow.io** : sélection d'un équipement RS485 configuré ;
  la communication emprunte le séquenceur RS485 commun.
- **Configuration manuelle** : débit, parité et bits d'arrêt spécifiés.

Le timeout et la politique d'erreur restent disponibles pour les ports actifs.
Les données sont désormais des variables **ConfigStore persistantes**, plutôt
que les anciennes structures NVS `mbtcp0/1/2` ; celles-ci sont importées au
premier démarrage si aucune nouvelle valeur n'existe encore. Les paramètres
effectifs du serveur changent **au prochain redémarrage** de Flow.io.

Sur MQTT, Flow.io publie, avec QoS 1 et retained, les paramètres de chaque
port dans les topics usuels :
`<base>/cfg/modbustcp/502`, `<base>/cfg/modbustcp/503` et
`<base>/cfg/modbustcp/504`.

Les **21 entités Home Assistant modifiables** sont enregistrées dans le
service Discovery **existant** (pas un second système). Par port :
- 4 listes de choix : mode RS485, parité, bits d'arrêt, politique d'erreur.
- 3 valeurs numériques : équipement, débit RS485, timeout en ms.

Leurs commandes MQTT utilisent le topic **`<base>/cfg/set`** et les validations
ConfigStore existantes ; la publication d'état confirme la sauvegarde.
Une modification ne redémarre pas automatiquement le contrôleur, afin de
ne pas interrompre le bus RS485 ni les fonctions de sécurité. **Redémarrer
manuellement** pour appliquer une modification aux listeners actifs. Ne pas
confondre l'état configuré publié sur MQTT avec l'état de fonctionnement actif
avant redémarrage.

## Correction du retour TCP et contrôle après flash

Le serveur répond via une écriture `send(..., MSG_DONTWAIT)` limitée à
128 octets par tour. Ne jamais utiliser `WiFiClient::availableForWrite()`
comme condition : la classe `NetworkClient` du framework Arduino-ESP32
n'implémente pas cette méthode, qui retourne toujours zéro par défaut.

Tester **le port 502 uniquement** tant que 503 et 504 sont désactivés :

```sh
python3 scripts/tests/modbus_tcp_load.py --host 192.168.1.71 \
  --target 502:17:500:28:3 --requests 5 --timeout 10
```

Remplacer `17`, `500`, `28` si l'équipement sélectionné n'accepte pas
ce bloc. En mode « Équipement Flow.io », le serveur conserve dans la
réponse TCP l'Unit ID demandé mais utilise sur RS485 l'adresse **fixe**
de l'équipement sélectionné, visible dans le message MQTT enrichi
(`rtu_unit_id`, `device_label`, `serial_baud_effective`).
En mode manuel, `rtu_unit_id=0` dans ces métadonnées signifie que
l'Unit ID du client TCP est utilisé pour les requêtes.

Dans **Network → Modbus TCP → TCP 502**, choisir « Exception Modbus »
pour faciliter le diagnostic et **redémarrer Flow.io** avant de tester :
les changements Config Store ne sont pas appliqués aux listeners existants.
Le JSON `GET /api/modbus/ports` précise désormais `active_error_mode`,
`pending_restart` et, dans `stats`, `resolver_failures`,
`tx_wouldblock`, `tx_errors`, `tcp_bytes_sent`, `requests`,
`responses`, `exceptions` et `timeouts`.

Pour identifier rapidement la phase en défaut après quelques requêtes :
- `requests=0` : requêtes TCP non décodées ou non reçues.
- `resolver_failures>0` : équipement Flow.io introuvable / profil série
  invalide (vérifier `device_label`, `device_id`, `rtu_unit_id`).
- `requests>0` et `tcp_bytes_sent=0` : aucune réponse envoyée.
- `tx_errors>0` : erreur d'écriture socket.
- `tcp_bytes_sent>0` : le code a transmis des octets à lwIP ; vérifier
  ensuite l'application cliente si elle ne les voit pas.

## Diagnostic après redémarrage de la passerelle RTU/TCP

Une passerelle qui redémarre (OTA) peut laisser une ancienne connexion TCP
persistante sans fermeture FIN/RST visible côté Flow.io. Avec **deux sockets
maximum par port**, les nouveaux clients peuvent se connecter puis être
immédiatement rejetés, sans avoir reçu de réponse Modbus.

La récupération automatique s'applique **seulement lorsque le port est plein
et qu'un nouveau client se présente** : Flow.io libère au plus une session
complètement inactive depuis **30 secondes**, en excluant les trames
partielles, transactions RS485 en cours et réponses TCP en cours.
Une connexion persistante régulièrement utilisée n'est donc pas évincée.
Le nombre maximal de clients reste **deux par port**.

Contrôler `GET /api/modbus/ports` (session authentifiée), port 502 :
- `stats.clients` : clients actuellement admis (maximum 2).
- `stats.connections_rejected` : connexions refusées parce que le port est plein.
- `stats.idle_reclaimed` : connexions inactives évincées pour admettre un
  nouveau client.
- `stats.requests`, `stats.tcp_bytes_received`, `stats.tcp_bytes_sent` :
  vérifier que les requêtes et réponses traversent réellement la passerelle.

La ligne série `[MODBUS-TCP] port=502` affiche également `idle_reclaimed`.
Pour isoler rapidement un éventuel problème d'anciennes sessions, redémarrer
**Flow.io uniquement** : si les réponses reprennent immédiatement, comparer
les compteurs du port 502 avant/après et examiner les clients persistants.
Le correctif ne modifie ni les timings du séquenceur RS485 ni le Wi-Fi/HTTP.

## Périmètre

Le module `ModbusTcpServerModule` expose trois ports TCP fixes **502, 503 et 504**,
avec **deux connexions entrantes par port** (six au total). Chaque requête valide
est acheminée sans cache par le service `ModbusMasterService`, donc par
`Rs485TransactionScheduler`. Les deux modes d'erreur sont *exception Modbus*
et *silence*. Les paramètres série peuvent venir d'un équipement RS485 Flow.io
ou d'une configuration manuelle par port.

Les tailles désormais autorisées en **Modbus RTU standard** sont FC03/FC04 :
**125 registres** ; FC16 : **123 registres** ; FC06 : un registre. Le protocole
constructeur non standard n'hérite pas automatiquement de ces maxima.

## Test sur matériel (sans écriture)

Après compilation/flashing **du firmware ET du SPIFFS correspondant**, activer
les trois ports dans l'interface et redémarrer si demandé. Choisir pour
chaque port un identifiant TCP, une fonction FC03 ou FC04, et une plage de
registres que l'équipement relié accepte réellement. Ne pas utiliser une
plage arbitraire, surtout sur les appareils utilisant un protocole constructeur
comme AquaGem : la passerelle transmet ici du **Modbus RTU standard**, elle
n'émule pas automatiquement les fonctions propriétaires AquaGem.

Depuis un ordinateur du même réseau :

```sh
python3 scripts/tests/modbus_tcp_load.py --host IP_DE_FLOWIO \
  --target 502:11:52:97:3 \
  --target 503:11:500:24:3 \
  --target 504:11:1000:2:3 \
  --requests 8 --pace-ms 100 --timeout 12 --probe-third
```

**Attention :** les valeurs d'adresse et d'ID ci-dessus ne sont que des
exemples de syntaxe ; les remplacer par des plages autorisées sur **chaque
équipement effectivement associé à chaque port**. La syntaxe d'un `--target`
est `PORT:UNIT_TCP:ADRESSE:NB_REGISTRES:FONCTION`. Une cible par port est
obligatoire. Le client ouvre deux connexions persistantes par port, ne transmet
que des FC03/FC04, vérifie l'en-tête MBAP et la longueur des réponses, et produit
par port : succès, exceptions Modbus, erreurs TCP, moyenne, P50, P95 et maximum
des temps de réponse. `--probe-third` sonde si une troisième connexion par
port est rejetée ; une absence de fermeture dans les deux secondes est
**inconclusive** à cause du backlog TCP et n'est pas une admission confirmée.

Pour tester un bloc de 125 registres, configurer `COUNT=125` **seulement si
le slave expose réellement 125 adresses consécutives**. Une exception Modbus
0x02 (adresse illégale) signifie généralement que l'équipement refuse la
plage, pas que le décodeur TCP limite encore à 32 registres.

## Charge soutenue et indicateurs

Exemple progressif, une fois les lectures courtes stables :

```sh
python3 scripts/tests/modbus_tcp_load.py --host IP_DE_FLOWIO \
  --target 502:11:52:97:3 \
  --target 503:11:500:24:3 \
  --target 504:11:1000:2:3 \
  --requests 100 --pace-ms 200 --timeout 15
```

Pendant l'essai, relever toutes les minutes la ligne série
`[MODBUS-TCP]` : `heap_internal`, `min_internal`,
`largest_internal`, `frag_est_percent`, `psram`,
`largest_psram`, `stack_hwm`, `rtu_queue`, `rtu_peak`,
`rtu_rejected`, `rtu_crc`, `rtu_timeouts`. Les lignes suivantes
détaillent, pour chaque port, les clients actifs, requêtes, réponses,
exceptions, refus, temps TCP maximum, attente en queue maximum et durée RTU
maximum. La route de diagnostic Modbus `GET /api/modbus/ports` est également disponible ; la configuration est maintenant dans Config Store. L'API diagnostique interroge aussi
`GET /api/modbus/ports` (session web authentifiée) et contient des
statistiques par port avec moyenne des temps réseau, de file et de RTU.

Ne pas interpréter une hausse des latences pendant six lectures RS485
simultanées comme une concurrence UART : le bus **reste sérialisé**.
Vérifier surtout qu'il n'y a pas de croissance persistante des allocations,
de perte de réponses, de famine des commandes de sécurité ou de saturation
prolongée de la file. Comparer les mêmes compteurs avant et après l'essai.

La réussite d'un test TCP sur ordinateur ne remplace pas les vérifications sur
matériel des interactions Polytropic/AquaGem, des vitesses série différentes,
du timeout configuré pour chaque port et des fonctions d'écriture FC06/FC16
(à tester à part, avec une procédure sûre et des registres non dangereux).

## Tests hôtes inclus

- `test/host/modbus_tcp_adu.cpp` : MBAP, FC03/FC04 125, FC16 123, dépassement.
- `test/host/modbus_tcp_bridge.cpp` : transfert d'une lecture 125 et écriture 123,
  retour d'exception, délais, écho du Unit ID TCP.
- `test/host/rs485_scheduler.cpp` : codec RTU avec trames maximales, CRC,
  coexistence des propriétaires et priorité sécurité.
- `test/host/modbus_tcp_sessions.cpp` : six sessions, trames partielles,
  admission de deux clients maximum par port.

Le script `scripts/tests/modbus_tcp_load.py` accepte désormais de 1 à 3 ports activés et utilise uniquement la bibliothèque
standard de Python 3 ; il ne nécessite ni pymodbus ni Home Assistant.


## Libération des sessions TCP et clients fantômes

Flow.io distingue désormais les fermetures reçues et les disparitions
brutales de clients :

- **FIN/RST explicite** : contrôle non bloquant `recv(MSG_PEEK |
  MSG_DONTWAIT)` toutes les **250 ms**, **avant** d'accepter une nouvelle
  connexion. Un FIN d'une session libre libère immédiatement le slot.
  Si une transaction Modbus est encore en cours lors d'une demi-fermeture
  TCP, Flow.io termine sa réponse en cours avant de libérer le slot.
- **Client disparu sans FIN/RST** (câble réseau retiré, redémarrage
  brutal d'une passerelle) : les nouveaux sockets activent le keepalive
  lwIP avec **10 s d'inactivité**, **5 s entre sondes** et **3 sondes**.
  La détection n'est donc **pas instantanée** : elle dépend des délais
  et du comportement TCP de lwIP et du réseau. Une connexion persistante
  en bonne santé n'est pas fermée pour simple inactivité.
- **Port saturé** : le mécanisme existant de récupération d'une session
  inactive depuis 30 s reste un dernier recours, au maximum une session
  récupérée par nouvelle connexion, jamais pendant une transaction en cours.

Diagnostics ajoutés dans `GET /api/modbus/ports` et dans la ligne
`[MODBUS-TCP]` : `peer_closed` compte les sessions libérées après
détection de déconnexion TCP ; `keepalive_setup_failures` compte les
échecs d'activation des options keepalive. Les compteurs préexistants
`clients`, `idle_reclaimed` et `connections_rejected` restent disponibles.

**Tests matériels :**
1. Ouvrir deux clients persistants sur le port 502. Vérifier `clients=2`.
2. Fermer proprement un client, vérifier `clients=1` et l'augmentation
   de `peer_closed`, puis ouvrir un nouveau client sans attendre 30 s.
3. Fermer les deux, vérifier `clients=0`. Refaire sur 503 et 504.
4. Couper brutalement l'alimentation d'une passerelle cliente de test
   sans fermeture FIN/RST. Surveiller la décroissance du nombre de clients
   lorsque les sondes keepalive échouent ; distinguer la latence normale
   d'expiration TCP d'une fuite définitive.
5. Pendant une transaction RS485 valide, vérifier qu'une demi-fermeture TCP
   ne coupe pas le travail du séquenceur avant la réponse.
6. Comparer les compteurs réseau, les files d'attente RS485, la pile et
   la RAM sous charge. Un `keepalive_setup_failures > 0` indique que les
   délais précis de keepalive ne sont pas garantis.

Ce correctif ne modifie ni les 2 connexions entrantes autorisées par port,
ni le protocole RS485, ni les mécanismes de sécurité Polytropic/AquaGem.
