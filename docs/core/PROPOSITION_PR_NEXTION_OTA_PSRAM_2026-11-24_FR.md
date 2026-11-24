# Proposition de Pull Request — Nextion PSRAM / UART2 / priorité Modbus

**Titre recommandé :** `fix(nextion): OTA sécurisée en PSRAM, UART2 dédié et priorité sur Modbus`

**Source :** `stephdust:fix/nextion-psram-uart-priority-20261124`  
**Destination :** `christophebelmont/Flow.io-Waveshare:dev`

## Résumé

Fiabiliser la mise à jour Nextion sur ESP32-S3, la rendre prioritaire sur les communications Modbus et garantir l'exclusion mutuelle avec l'OTA couplée firmware + SPIFFS. **Une mise à jour Nextion a été confirmée réussie sur le matériel de l'utilisateur après compilation.**

## Corrections

- Correction du chargeur Nextion utilisant `Serial1` (RS485) : transfert sur `Serial2` (HMI), attente série coopérative et gestion des erreurs UART.
- Préchargement de l'intégralité du `.tft` en PSRAM (2 Mio de réserve), jusqu'à trois tentatives HTTP avec reprise Range contrôlée et contrôle de taille.
- **SHA-256 du manifeste obligatoire** avant d'initialiser l'écran pour le flash.
- Vérification des ACK `0x05` après chaque bloc de 4 Kio, y compris le dernier, sans forcer le redémarrage de l'écran après un transfert incomplet.
- Exclusivité réciproque Nextion ↔ OTA firmware + SPIFFS, y compris opérations locales, distantes et en attente.
- Suspension de Modbus TCP et des nouvelles transactions RS485 pendant la mise à jour, sans interrompre une transaction série déjà active.
- Progression réelle par phase dans l'interface web FR/EN, suppression du 5 % artificiel.

## Validation

- ✅ Succès de la mise à jour Nextion sur le matériel de l'utilisateur.
- ✅ 4 495 simulations déterministes sans échec du modèle et 44/44 contrôles statiques.
- ⏳ À confirmer par Christophe : essais de concurrence dans les deux sens, modèles Nextion additionnels, perte HTTP/ACK UART, saturation Modbus, reprise post-erreur et watchdog.

Les simulations ne remplacent pas ces essais matériels.

## Documents joints à la branche

- [Rapport PDF français pour Christophe](../reports/RAPPORT_NEXTION_OTA_PSRAM_CHRISTOPHE_2026-11-24_FR.pdf)
- [Changelog français](CHANGELOG_NEXTION_PSRAM_OTA_PRIORITE_2026-11-24_FR.md)
- [Rapport de simulations](nextion-psram-ota-failure-matrix-2026-11-24.md)

Aucun changement des calculs ni du protocole métier Modbus. Travaux `famille5` exclus du périmètre.

---

**Note opérationnelle :** la création automatique de la PR vers le dépôt de Christophe a renvoyé `403 Resource not accessible by integration`. Cette proposition est prête à copier dans la PR ouverte depuis le [comparateur GitHub](https://github.com/christophebelmont/Flow.io-Waveshare/compare/dev...stephdust:fix/nextion-psram-uart-priority-20261124?expand=1).
