# OTA SPIFFS — sécurité et validation avant généralisation

> **Statut : plan de durcissement, non implémenté intégralement dans cette PR.**
> La télémétrie OTA et la pause des travaux Web avant clonage sont proposées pour revue.
> Elles ne constituent ni un rollback SPIFFS ni une protection contre toutes les coupures.

## Périmètre de la PR

Cette proposition apporte la télémétrie RAM/PSRAM/piles, les réglages AsyncTCP,
l'optimisation de certaines requêtes Web, l'export TXT des logs et l'instrumentation OTA.
Elle ne modifie ni le serveur Modbus ni les scripts d'optimisation de compilation.

## Risque principal observé dans le code source

Le chemin `runSpiffsUpdate_` efface puis réécrit la partition SPIFFS active,
ce qui peut laisser les ressources Web indisponibles si l'alimentation tombe
ou si le téléchargement échoue pendant l'écriture. La copie de la partition
SPIFFS active vers la partition inactive lors d'un OTA firmware est une opération
différente : son existence n'assure pas la sûreté de l'OTA SPIFFS autonome.

## Améliorations à réaliser — priorités

### P0 : intégrité et atomicité
1. Ne jamais effacer la partition SPIFFS actuellement utilisée par le firmware.
   Préparer l'image dans une partition inactive, puis vérifier son intégrité avant
   de rendre le nouveau couple firmware/SPIFFS actif.
2. Valider taille attendue, partition cible, SHA-256 de l'image complète et
   authenticité de la source (signature/manifest signé selon la politique projet).
   Un transport HTTP local et un checksum seuls ne prouvent pas l'authenticité.
3. Conserver l'ancien couple firmware/SPIFFS tant que la nouvelle version n'a pas
   passé les contrôles de démarrage et de compatibilité. Ne pas réutiliser une
   ancienne image SPIFFS avec un firmware incompatible.
4. Définir et persister un état de transaction (`idle`, `staging`,
   `verified`, `pending_boot`, `confirmed`, `rollback`) tolérant aux resets.
   La bascule doit être ordonnée et récupérable.

### P1 : concurrence, ressources et accès
5. Sérialiser les opérations OTA firmware et SPIFFS, interdire deux mises à jour
   concurrentes, et suspendre les travaux périodiques qui accèdent à la flash
   pendant l'effacement et la copie.
6. Maintenir les mesures de marge de pile, taille du plus grand bloc interne,
   fragmentation indicative, durées de flash et progression ; borner les buffers.
7. Prévoir un timeout par étape et la remise en état sur toutes les sorties
   d'erreur ; les interfaces HTTP de progression doivent rester utilisables.
8. Restreindre le déclenchement OTA aux sessions autorisées, protéger l'origine
   du manifest et utiliser TLS et/ou des signatures lorsque le déploiement le permet.

### P2 : récupération et qualification
9. Surveiller le montage SPIFFS et la disponibilité des fichiers Web essentiels
   au boot ; confirmer la nouvelle version seulement après ces tests.
10. Prévoir une procédure de récupération documentée (ancienne image,
    partition de secours ou reflash physique) et des journaux diagnostiques
    permettant de déterminer quelle partition reste valide.

## Tests requis avant merge

- Mise à jour nominale firmware puis SPIFFS, avec contrôle de l'UI après reboot.
- Coupure d'alimentation simulée **sur carte de validation uniquement** :
  avant effacement, pendant effacement, au milieu de l'écriture, pendant
  vérification et au moment de la bascule.
- Mauvais hash, image tronquée, timeout HTTP, manifeste invalide, partition
  trop petite et échec de montage.
- Rafales de sessions HTTP/WebSocket, OTA répétées, charge RAM/PSRAM et
  surveillance des high-water marks des tâches Web et AsyncTCP.
- Compatibilité version firmware/SPIFFS et retour fonctionnel à l'état antérieur.

## Limites explicites de ce transfert

- Le correctif de pause Web anticipée vient d'une branche expérimentale ;
  il doit être compilé puis qualifié séparément sur ESP32-S3.
- Le pré-calcul de l'empreinte SPIFFS avant `server_.begin()` provient d'un
  correctif Web isolé ; vérifier l'effet sur le démarrage et la latence HTTP.
- Aucun rollback atomique SPIFFS, contrôle SHA-256 ou signature de manifest
  n'est ajouté par cette PR. Ces protections nécessitent une implémentation
  dédiée et des tests destructifs sur un équipement non productif.
