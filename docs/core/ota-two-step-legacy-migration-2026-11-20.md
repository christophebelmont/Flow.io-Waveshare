# Migration OTA A/B en deux étapes depuis une version stable

## Origine et périmètre

**Base contrôlée :** `feature/sysmon-safe-diagnostics-20261120`, commit `897ca08843ec30258a7594aee620db357ae2d999`, avant les changements expérimentaux de l'OTA SPIFFS. Cette branche est indépendante des précédentes expérimentations d'OTA automatique et n'en reprend pas le mécanisme de reconnection Wi-Fi.

**Objectif :** permettre aux utilisateurs dont l'interface affiche encore deux boutons **Firmware** et **SPIFFS** d'effectuer une migration *sans USB et sans /rescue*, puis de disposer d'un bouton **FlowIOS3 (firmware + SPIFFS)** et d'un bouton Nextion.

## Procédure utilisateur initiale

1. Depuis l'ancienne interface, choisir la version de transition `flowios3-VERSION.bin` et cliquer **Firmware**. Le **moteur historique**, déjà installé, copie le SPIFFS courant vers le slot associé à la nouvelle application, écrit le nouveau firmware dans le slot inactif, puis redémarre. **Ne pas cliquer sur SPIFFS avant ce redémarrage.**
2. Le nouveau firmware démarre avec le SPIFFS historique, vérifié par son `/release.json` et les ressources Web essentielles. Il ne lance **aucune mise à jour automatique**. L'ancienne interface reste donc visible.
3. Depuis cette ancienne interface, cliquer **SPIFFS** pour *finaliser la migration*. Ce bouton ne fait plus d'OTA SPIFFS indépendante : le firmware de transition convertit l'URL historique `flowios3-spiffs-VERSION.bin` en `flowio-release-VERSION.json`. La version doit être **identique** à celle du firmware de transition.
4. Le nouveau moteur télécharge ce manifeste et les deux binaires versionnés référencés, écrit le SPIFFS et le firmware dans les **slots inactifs**, valide les tailles, SHA-256, métadonnées (`Flow.IO`, version, matériel) et la présence de `/ota-paired-v1.marker`, puis effectue le commit A/B et redémarre.
5. Le nouveau SPIFFS contient l'interface moderne : **une seule ligne FlowIOS3 (Firmware + SPIFFS OTA A/B)**, plus Nextion. Les opérations distantes ultérieures utilisent le manifeste versionné.

La migration se déroule normalement en **deux clics et deux redémarrages**. Le premier saut dépend des capacités OTA de l'ancienne version (non transactionnel pour le couple). La seconde opération est une transaction A/B groupée.

## Sécurité / échec

- **Aucun chemin d'écriture directe du SPIFFS actif via `runSpiffsUpdate_`.** Le bouton historique sert exclusivement de déclencheur de la transaction groupée et n'est accepté qu'en présence d'un SPIFFS ancien valide.
- Si le firmware de transition échoue à monter un SPIFFS ancien valide, les protections de démarrage existantes continuent de s'appliquer ; un retour arrière ESP-IDF peut intervenir.
- Le firmware de transition accepte temporairement un SPIFFS historique **cohérent avec lui-même**, même lorsque son numéro de version diffère de celui du firmware de transition. Cette concession est limitée au démarrage transitoire.
- Si le téléchargement, une empreinte ou les métadonnées échouent pendant la finalisation, le couple actuellement exécuté reste actif ; le moteur n'active pas de nouveau slot.
- La transaction locale et distante exige que le SPIFFS cible soit **une image nouvelle groupée**, avec marqueur, et que les partitions cibles n'empiètent pas sur les partitions actives.
- Le bouton **SPIFFS** ne peut plus être utilisé après une migration réussie. Un appel direct `/fwupdate/spiffs` est alors rejeté.
- L'OTA Nextion reste séparée.
- Les modifications Wi-Fi et les optimisations mémoire/stack sont celles de la base stable `897ca088`, sans portage des essais Wi-Fi récents.

## Distribution et avertissements

Pour chaque version publier **ensemble** dans le répertoire HTTP `binary/` :

- `flowios3-VERSION.bin`
- `flowios3-spiffs-VERSION.bin`
- `flowio-release-VERSION.json`
- `release.json` (alias de la dernière release)
- `manifest.json` (catalogue historique)
- `flowio-VERSION.zip` (option upload groupé local)

La compilation locale doit régénérer les deux binaires et leurs empreintes SHA-256. Un ancien binaire conservé sous le même numéro de version `2026.11.1` est **incompatible** avec un nouveau manifeste. Ne pas tester ni publier un mélange de générations de fichiers.

Pour le suivi, `GET /fwupdate/status` expose `app_slot`, `boot_slot`, `spiffs_valid`, `migration_pending` en plus de l'état existant.

**STATUT : CODE NON COMPILÉ ET NON TESTÉ SUR MATÉRIEL.** Tester les migrations depuis les versions stables réellement distribuées, sur les deux sens A→B et B→A, avec pertes de connexion et interruptions d'alimentation contrôlées. Ne pas annoncer comme universel ni déployer en production avant validation du premier saut historique, du démarrage transitoire, de la finalisation A/B et du rollback.
