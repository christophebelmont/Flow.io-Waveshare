# Acceptation et application des consignes RS485

La qualité d'une lecture et l'état d'une commande sont indépendants. Une lecture
`Confirmed` est valide et récente ; elle peut encore décrire la vitesse initiale
pendant que l'équipement attend avant d'appliquer une nouvelle consigne.

Pour un pilote série avec retour de lecture :

- `Pending` : la dernière commande n'a pas encore été acquittée intégralement.
- `Confirming` : la commande est acquittée, mais les lectures postérieures ne
  confirment pas encore son application physique.
- `Applied` : les lectures sont valides, l'état marche/arrêt correspond et la
  consigne observée rejoint la consigne normalisée envoyée au protocole, à une
  demi-unité de résolution du registre de retour près.
- `Failed` : une opération a échoué ; une lecture différente à elle seule ne
  constitue pas une erreur de communication.
- `Frozen` : les écritures sont suspendues.

Pour un arrêt utilisant un registre combinant marche et vitesse, seul l'état
arrêté est attendu : la vitesse de retour à l'arrêt ne représente pas la
consigne préparée. Un registre de consigne indépendant reste comparé, même à
l'arrêt. Sans retour de lecture, l'acquittement conserve la qualité `Estimated`.

Le démarrage AquaGem à 2900 rpm pendant 60 secondes est géré par l'équipement.
Le profil Flow.io initialise également sa consigne à 2900 rpm. L'état marche
est confirmé dès la première lecture valide du bit `Pump On`, indépendamment
de la convergence de la vitesse. Une consigne choisie ensuite reste conservée.
Flow.io conserve la commande acquittée et poursuit ses lectures habituelles,
sans temporisation JavaScript ni réémission automatique liée à l'écart de vitesse.
Une nouvelle demande remplace la précédente sans séquence arrêt/redémarrage.
Le passage au vert dépend de la lecture effective, pas du seul écoulement de
60 secondes. Si une vitesse valide reste différente au-delà, la commande reste
en attente ; aucun délai d'échec métier supplémentaire n'est inventé.

L'API des options équipements fournit `setpoint` (demande), `acceptedSetpoint`
(valeur normalisée acquittée, uniquement dans les phases `Confirming` et
`Applied`) et `observedSetpoint` (lecture valide), avec `phase` et `quality`.
La fenêtre de détail reconstruit donc une attente même après réouverture.

Dans l'interface, un brouillon n'est envoyé que par le bouton. Pendant l'attente,
la dernière lecture apparaît à gauche de la cible, accompagnée du snake orange
et de « Commande acceptée · Application en attente ». Si la lecture confirme déjà la marche
demandée, le message devient « Marche confirmée · Consigne en attente » :
l'attente concerne la consigne et non la mise en marche. Une nouvelle saisie reste
possible lorsque l'équipement tourne. Une requête HTTP refusée conserve sa
valeur et son erreur locale, sans réutiliser la confirmation verte précédente.
Après convergence, une normalisation différente de la demande est expliquée
par « Demandé : … · Retenu : … ».

Les tests `serial_register_profiles.cpp` simulent le démarrage de 60 secondes,
une demande de remplacement à mi-parcours, les lectures divergentes et les
arrêts avec consigne indépendante. `test_modbus_confirmation_dialog.cjs` couvre
les rafraîchissements, les remplacements, les refus, les snapshots retardés et
la réouverture de la fenêtre.

## Incident de communication

Une erreur de transport conserve la demande et la dernière mesure horodatée.
La mesure devient indisponible pour le pilotage. Le dialogue affiche la qualité
de communication et l'âge de la dernière observation. L'arrêt opérateur remplace
la révision en cours, annule les requêtes obsolètes et reste demandé jusqu'à
confirmation. Une trame déjà émise termine son échange avant le suivant.

Pour les profils avec retour, une écriture sans acquittement déclenche une
lecture avant toute réémission. Après une panne de lecture, le pilote relit
l'état et la consigne utiles et ne réapplique que la demande encore autorisée.
La confirmation Polytropic compare également le mode du registre 1000.
Une demande ON qui ne peut être confirmée sous 15 s expire. La vitesse de
l'AquaGem peut continuer à converger après confirmation de marche. Une demande
expirée exige une nouvelle action. Les sondages continuent pendant la panne,
avec des intervalles de récupération de 1, 2, 4 puis 8 secondes.

Quand une limite quotidienne est configurée, la durée série sans observation
valide est imputée au budget de sécurité et persistée séparément du temps
de marche mesuré. Les anciens compteurs sont repris avec un budget inconnu nul.

La file réserve une place à la surveillance et une à l'arrêt. L'échéance d'une
transaction couvre la file, la transmission et les tentatives. Les journaux
IOModule indiquent l'attente avant chaque transmission et le temps de réponse.
Les journaux PoolDevice identifient la source de la demande, la révision, le
motif d'inhibition et la marche effectivement relue.

La PAC utilise les dépendances de filtration configurées ou, en leur absence,
le contact de débit du registre 500. Un défaut FLO (501, bit 14) inhibe le
chauffage jusqu'à sa disparition constatée. Une nouvelle chauffe exige une
preuve actuelle de circulation. Le paramètre Polytropic
`verified_flow_grace_ms` vaut 0 par défaut. Après vérification sur site de la
protection locale de débit, une valeur de 1 à 60 000 ms permet de maintenir
une chauffe déjà engagée jusqu'à cet âge de la dernière preuve. Un arrêt
demandé à la pompe, un débit insuffisant confirmé ou FLO l'emportent sur ce
délai. Contrôler cette politique sur installation avant d'autoriser le maintien.

Le profil Waveshare expose un port RS485 isolé. Les deux machines partagent
ce port et un défaut électrique peut encore couper les deux liaisons.
