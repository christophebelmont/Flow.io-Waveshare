# Commandes partagées sur les équipements RS485

Implémentation expérimentale sur la branche `feature/modbus-server`. Les
interverrouillages de Flow.io restent prioritaires sur les commandes externes.

## Politique de commande

- Au démarrage, Flow.io applique la consigne configurée. Tant que cette
  demande initiale n'a pas été **confirmée par lecture**, elle reste
  prioritaire et peut être retentée à cadence limitée si la dépendance
  RS485/interverrouillage devient disponible. Une demande rejetée par le
  service est également retentée. Aucune écriture ne doit contourner les
  interverrouillages de sécurité.
- Après confirmation, Flow.io ne cherche plus à réimposer son ancienne
  consigne lorsqu'un afficheur externe prend la main.
- Après confirmation, une modification physique constatée dans des lectures
  **fraîches et valides** est adoptée comme état désiré, sans écriture de
  correction. Une nouvelle demande Flow.io reprend la main.
- Le diagnostic `applied` décrit la dernière commande envoyée par Flow.io ;
  `observed` l'état physique ; `desired` peut suivre un changement externe.
- Les changements de configuration constatés sur le matériel sont persistés
  **uniquement lorsqu'ils diffèrent** (limitation des écritures NVS).
- Un échec/timeout de confirmation déclenche un diagnostic `Expired` : Flow.io
  accepte ensuite le retour physique plutôt que réémettre indéfiniment.
- Le contrôleur automatique PoolLogic n'émet plus de commande de rattrapage
  toutes les 5 s sur les RS485 équipés d'un retour de lecture. Ses changements
  de demande restent des commandes explicites. Les conditions de sécurité
  conservent une voie d'arrêt et de réessai prioritaire.

## Profils

### Polytropic
- Lire consigne 1001 et état/mode 1000. Le mode est reconstruit à partir des
  quatre bits de poids faible lorsqu'il fonctionne ; à l'arrêt, le dernier
  mode configuré est préservé.
- Ne plus réparer une consigne différente après confirmation initiale.
- Conserver la validation des limites (registre 58) pour une nouvelle écriture.

### AquaGem
- La première réponse valide (ou la première réponse après une perte de
  communication confirmée) démarre une garde **de 60 secondes**. Ce n'est
  qu'une approximation prudente de la remise sous tension : aucun signal
  matériel indépendant de power-on n'est disponible ici.
- Pendant cette garde, les lectures sont permises ; les commandes Flow.io
  restent en attente. Si deux commandes arrivent, la plus récente remplace
  la précédente. Le timeout de livraison de 15 s ne démarre **qu'après**
  la garde de 60 s, jamais pendant celle-ci.
- Le protocole remonte des RPM **mesurés**, non une consigne distincte.
  Le logiciel ne peut donc pas connaître avec certitude la consigne exacte
  saisie sur l'afficheur. Après ACK et retour d'état marche confirmé, une
  tolérance de 75 RPM permet de confirmer approximativement l'application
  d'une petite correction (par exemple 1650 RPM lus pour 1670 RPM demandés).
  L'écran continue d'afficher la vitesse mesurée réelle et non une fausse
  valeur de 1670 RPM. L'adoption de la vitesse est une estimation :
  quantification par 50 RPM et stabilité continue de 15 s, avec un écart
  minimal de 75 RPM. Aucune adoption de la vitesse pendant la rampe.
- L'état marche/arrêt provient du statut AquaGem et peut être synchronisé
  indépendamment de l'estimation de vitesse.

### Autres équipements RS485
- Si une lecture de statut fiable existe, l'état marche/arrêt peut être adopté.
- Le sens d'un registre de feedback propre à un appareil doit être confirmé
  **avant** d'en faire une consigne persistée. Pas de conversion implicite
  d'une mesure physique en une consigne sur les profils personnalisés.
- Les équipements sans feedback conservent leur ancienne stratégie d'écriture
  (pas de détection sûre d'une modification externe).

## Vérifications matérielles demandées

1. Flow.io redémarre, applique sa consigne au Polytropic, puis confirme sa
   lecture. Modifier ensuite la température et le mode sur l'afficheur :
   Flow.io doit adopter le nouvel état sans réécrire l'ancien.
2. Changer la consigne Flow.io : une seule écriture logique, puis vérification.
   Répéter avec une déconnexion/reconnexion du bus.
3. AquaGem : remise sous tension réelle, observation des lectures pendant
   60 s, **aucune** écriture Flow.io avant la fin de la garde. Une seconde
   demande pendant la garde doit supplanter la précédente.
4. AquaGem : variation RPM de la pompe, puis commande externe stabilisée ;
   vérifier que l'estimation n'enregistre pas de RPM de démarrage.
5. Vérifier blocages de débit, arrêts de sécurité, pertes de communication,
   commande contradictoire, retour en ligne, trafic Modbus TCP simultané.
6. Mesurer charge du séquenceur RS485 (multi-baud), pile et mémoire pendant
   plusieurs heures ; mesurer également les mises à jour NVS.
