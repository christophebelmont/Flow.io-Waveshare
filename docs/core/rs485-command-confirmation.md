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
et de « Commande acceptée · Application en attente ». Une nouvelle saisie reste
possible lorsque l'équipement tourne. Une requête HTTP refusée conserve sa
valeur et son erreur locale, sans réutiliser la confirmation verte précédente.
Après convergence, une normalisation différente de la demande est expliquée
par « Demandé : … · Retenu : … ».

Les tests `serial_register_profiles.cpp` simulent le démarrage de 60 secondes,
une demande de remplacement à mi-parcours, les lectures divergentes et les
arrêts avec consigne indépendante. `test_modbus_confirmation_dialog.cjs` couvre
les rafraîchissements, les remplacements, les refus, les snapshots retardés et
la réouverture de la fenêtre.
