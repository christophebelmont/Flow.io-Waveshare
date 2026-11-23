# Flow.io — diagnostic NVS du dernier échec OTA et compteurs Modbus TCP

22 novembre 2026 — branche `experiment/modbus-tcp-psram-buffers-20261122`, PR #2.

## Objectif

En production, une tentative OTA a échoué alors que l'archive HTTP
dans le PCAP était intégrale ; la tentative suivante a réussi.
L'ancien `last_operation` est remplacé par le succès de l'opération
suivante et les journaux Web n'incluaient pas la fenêtre de l'échec.
Ne pas attribuer l'erreur à HTTP, Flash ou Modbus avant preuve.

## OTA : enregistrement indépendant

- Nouveau blob NVS `flowio/fw_up_fail`, typé/versionné et protégé
  par checksum, de moins de 160 octets. Aucune allocation dynamique.
- Champs : `operation_id`, cible, **étape capturée avant l'abandon**,
  catégorie de cause (`code`) et motif original (96 octets au plus,
  terminé par NUL). Une fois validé après reboot, il est publié dans
  deux lignes `[OTA-LASTFAIL]` du journal Web.
- Catégories : 1 autre, 2 réseau/HTTP, 3 manifeste,
  4 intégrité/SHA, 5 Flash/partition, 6 commit/reboot,
  7 timeout. **Ce n'est pas un code `esp_err_t`**. Si disponible,
  le détail ESP-IDF est conservé dans le motif.
- En cas d'OTA A/B distante, l'étape est capturée AVANT
  `abortLocalRelease_()` puis le diagnostic est écrit après
  l'abandon de la transaction. En cas d'erreur `failLocalRelease_`,
  la capture se produit avant `setStatus_(Error)` qui efface la phase.
- Déduplication par ID d'opération : les deux chemins
  `failLocalRelease_` et `runJob_` ne provoquent pas deux écritures
  NVS et ne remplacent pas la première cause par `stage=idle`.
- **Une réussite OTA ultérieure ne supprime pas ce blob**. Un nouvel
  échec le remplace. La persistance intervient uniquement sur le
  chemin d'erreur, jamais par bloc HTTP, pendant le SHA, l'effacement
  ou l'écriture Flash.
- `/api/fwupdate/status` tente d'ajouter `last_failure`
  (`operation_id`, `code`, `stage`, `reason`) si les budgets
  JSON et de réponse sont suffisants. Le statut OTA historique reste
  prioritaire ; la présence de l'objet est donc facultative.
  Le journal au boot reste le moyen garanti par la logique du
  module pour visualiser un enregistrement NVS valide.

## Modbus TCP : suspendre / reprendre sans log par paquet

- `otaSuspensions_` : nombre de fermetures effectives des listeners
  et clients TCP dues à une OTA (la tâche ne traite pas de nouveaux
  clients dès qu'elle observe `otaBusy=1`).
- `otaResumptions_` : nombre d'ouvertures effectives après la levée
  du verrou OTA et un `SocketServer::begin()` réussi.
- `[MODBUS-OTA] paused=... resumed=... pending=... busy=... listen=...`
  toutes les 5 secondes pendant l'OTA et chaque minute hors OTA.
  Compteurs cumulés depuis le boot, remis à zéro au redémarrage.
- **Ces chiffres ne comptent PAS les SYN TCP reçus** pendant que
  les ports sont fermés : ils ne sont pas accessibles à la couche
  applicative dans l'architecture actuelle. Un PCAP sur l'interface
  qui voit les ports 502/503 est nécessaire pour quantifier ces
  tentatives de reconnexion.

## Préservation de l'OTA prioritaire

Le téléchargement complet en PSRAM, les empreintes SHA-256,
`esp_ota_begin`, les écritures Flash, le commit A/B, le reboot,
le `CDC=0` et la logique de quiescence Modbus existante
restent inchangés. La sauvegarde NVS du dernier échec s'ajoute au
chemin où l'OTA est déjà en erreur.

## Validation

`node scripts/tests/check_ota_persistent_failure.mjs` puis
`node scripts/tests/check_ota_reboot_diags.mjs`,
`node scripts/tests/check_ota_error_semantics.mjs` et
`node scripts/tests/check_modbus_ota_priority.mjs`.

Sur ESP32-S3 de test : provoquer une erreur HTTP/Flash sans risque,
vérifier `[OTA-LASTFAIL] persisted=1`, lancer ensuite une OTA
complète, puis confirmer au nouveau boot `retained=1` avec l'ID
et le motif du premier échec. Vérifier que `[MODBUS-OTA]` reflète
la suspension et la reprise de clients sans bloquer le reboot.
Tester les modifications en laboratoire avant une nouvelle OTA
en production.
