# Crash au démarrage du 5 octobre 2026

La trace fournie identifie `Stack canary watchpoint triggered (poollogic)`.
Son empreinte ELF `e13a868bf` correspond au SHA-256 complet
`e13a868bfd332b16e1cbb0b25d732194c58b7b5af7b9c8b44bb883072a17ee1b`.
Le décodage a donc été effectué avec le binaire exact, conservé temporairement
sous `/tmp/flowio-crash-e13a868bf.elf` avant recompilation.

Le chemin est : `runControlLoop_` → `applyDeviceControl_` →
`writeDeviceDesired_` → `PoolDeviceModule::setRunning_` → `tickDevices_` →
`DigitalRelayDriver::tick` → IO → TCA9554 → Wire → pilote I²C ESP-IDF.
Le moteur d’expressions ne figure pas dans cette pile.

## Correction

Les métadonnées complètes `PoolDeviceSvcMeta` étaient conservées dans plusieurs
cadres actifs pendant la commande synchrone. PoolLogic extrait maintenant une
vue limitée aux champs de contrôle et de diagnostic. Le snapshot complet est
libéré avant d’appeler le pilote.

Les instructions `entry a1` du binaire ESP32-S3 donnent :

| Fonction | Avant | Après |
| --- | ---: | ---: |
| `writeDeviceDesired_` | 800 octets | 80 octets |
| `applyDeviceControl_` | 416 octets | 64 octets |

Ces deux cadres économisent ensemble 1 072 octets sur le chemin I²C.
Le nouveau lecteur de métadonnées a un cadre de 432 octets, terminé avant
l’écriture matérielle. La pile de la tâche reste fixée à 4 096 octets.

## Persistance NVS

Le même journal contient `nvs_available=23` et une erreur de calibration Wi-Fi
`0x1105`, soit `ESP_ERR_NVS_NOT_ENOUGH_SPACE` dans le SDK utilisé.
Cela établit un échec d’écriture par manque d’espace ; cela ne prouve pas à lui
seul que la sauvegarde de V00 a échoué.

`ConfigStore::applyJson` ignorait les résultats des écritures persistantes.
Il utilise désormais `writePersistent` et renvoie une erreur lorsque la sauvegarde
échoue. La modification reste en RAM et le journal le précise. Un marqueur conserve
la nécessité d’écriture : soumettre de nouveau une valeur identique à la RAM
réessaie la sauvegarde au lieu de renvoyer un faux succès. Les patches comportant
plusieurs champs ne sont pas transactionnels.

Ce correctif n’efface aucune clé et ne change pas les partitions. Le manque de
place NVS reste à résoudre séparément, après inventaire des données à conserver.
L’extrait ne montre pas de connexion MQTT ou de découverte HA terminée avant le
crash ; il ne permet pas d’établir précisément la cause de l’absence de V00.

## Vérification

- Compilation `Flowio-waveshare-esp32-s3` réussie.
- Mesures des cadres par désassemblage des deux ELF.
- Tests actionneurs : 8 scénarios réussis.
- Tests ConfigStore : export JSON, validation réelle d’expression et échec/reprise
  de persistance via la méthode de production `applyJson`, réussis.
- Tests valeurs, historique et expressions sous ASan/UBSan : réussis.

La stabilité sur carte et la conservation des paramètres après reboot restent
à vérifier avec le firmware corrigé. Le binaire a été produit, pas installé.
