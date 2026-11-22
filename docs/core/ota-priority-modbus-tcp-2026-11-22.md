# Flow.io — OTA prioritaire, Modbus TCP dégradé volontairement

Date : 22 novembre 2026.
Branche de validation : `experiment/modbus-tcp-psram-buffers-20261122` (PR #2).

## Décision de sûreté en production

La **priorité #1 est la réussite de la mise à jour OTA**, et non la
continuité Modbus TCP, MQTT ou Web. Les mesures sur PAC Polytropic et
AquaGem en production indiquent deux OTA A/B réussies et le pool
Modbus TCP `3120 bytes ready=1`. Elles ont aussi montré jusqu'à
3,7 secondes de latence autour du `server_.tick` pendant la préparation
Flash, et une marge de pile Modbus d'environ 772–776 octets.

Au lieu de changer le chemin OTA déjà validé (effacement initial avec
`esp_ota_begin`, téléchargement/contrôle SHA-256 complet en PSRAM,
écriture puis vérification Flash, commit A/B), **on ne modifie aucune
ligne du firmware updater**. L'effacement progressif
`OTA_WITH_SEQUENTIAL_WRITES` n'est pas activé : cela aurait introduit
une régression possible dans l'unique service prioritaire pour gagner
du temps de réponse Modbus.

## Modification Modbus (réversible)

- Pile `modbustcp` : 4096 -> **5120 octets en RAM interne**.
  Le budget supplémentaire est 1024 octets de DRAM ; ne pas déplacer
  la pile lwIP vers la PSRAM.
- Suppression de la construction `otaNetLine[384]` et de la ligne
  Serial redondante dans `loop()` : les statistiques OTA existantes
  `[OTA-NET]` et `[OTA-TCP]` restent envoyées dans LogHub/Web
  toutes les 5 secondes.
- Lorsque le service OTA reste occupé **au moins 300 ms**, le
  serveur Modbus ferme ses clients et ses listeners une seule fois,
  annule les transactions en attente via son chemin de fermeture
  existant, puis cesse toute activité TCP jusqu'à la fin de l'OTA.
  Ce délai laisse terminer normalement les petits contrôles
  `manifest.json` qui ne constituent pas une mise à jour Flash.
- Le pool de **3120 octets PSRAM** reste alloué lors du stop.
  Si l'OTA échoue sans reboot, le module revient au mode nominal et
  rouvre les ports activés au passage suivant.
- Les messages `[MODBUS-TCP] ota_priority=1 paused=1` et
  `ota_priority=0 resume=1` permettent d'identifier la période de
  suspension. Les timeouts/déconnexions clients sont **attendus** et
  ne constituent pas une erreur OTA.

## Diagnostic de résolution (sans changement de routage)

Le dernier échec de résolution par port mémorise l'unité MBAP,
la fonction Modbus, l'adresse et l'instant. Dans le snapshot Web
périodique, `[MODBUS-RESOLVE]` publie ces détails, l'indice de
device configuré, le mode (1=Flow.io device, 2=manuel), et le
compteur cumulatif `fails`. Cela permet de comprendre
`resolver=2 silent=2` sans introduire de fallback d'adresse ni
de requête imprévue vers un autre slave RS485.
Les erreurs sont toujours traitées selon la politique configurée :
exception standard ou silence.

## Validation exigée avant production

1. `node scripts/tests/check_modbus_ota_priority.mjs`
2. `node scripts/tests/check_ota_reboot_diags.mjs`
3. `node scripts/tests/check_modbus_tcp_web_diagnostics.mjs`
4. `node scripts/tests/check_modbus_tcp_psram_buffers.mjs`
5. Compilation PlatformIO Waveshare S3, USB CDC=0.
6. Avant OTA : deux ports ouverts et transactions RTU sur FC03/04.
   Au démarrage effectif de l'OTA : fermeture des clients et
   `[MODBUS-TCP] ota_priority=1` ; validation Flash et redémarrage.
   Au boot suivant : pool PSRAM réalloué, sessions normales.
7. Simuler si possible une OTA échouée avant commit : la tâche
   doit rouvrir ses listeners sans corruption des transactions.
8. Vérifier le watermark `modbustcp`, la DRAM interne et que
   `[BOOT-RESET] planned=ota_release stage=commit` reste valable.

Le changement ne garantit pas une OTA infaillible (pannes
d'alimentation/Flash/HTTP restent possibles) ; il réduit seulement
la concurrence du serveur Modbus pendant l'OTA.

Ne pas déclarer ce patch validé en production sans les essais 5–8.
