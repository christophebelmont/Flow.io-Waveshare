# Capture des journaux Flow.io

Ce répertoire contient deux collecteurs **externes** pour archiver les journaux de Flow.io depuis un poste Windows ou Linux. Ils n'entrent pas dans la compilation PlatformIO, ne sont pas déployés par OTA et ne modifient pas le fonctionnement de l'ESP32-S3.

Les scripts récupèrent les journaux runtime et les journaux de démarrage (boot), détectent les redémarrages via `boot_id` et mémorisent leur position de lecture. Ils continuent à écrire dans les mêmes fichiers au fil du temps, sans réimporter volontairement les anciens journaux.

## Configuration commune

Ouvrir le script correspondant à votre système et modifier, **avant le premier lancement**, les variables de configuration au début du fichier :

- `FLOWIO_IP` : adresse de Flow.io sur le réseau local (par défaut `192.168.1.50`, à adapter).
- `FLOWIO_LOGIN` : utilisateur de l'interface Web (par défaut `admin`).
- `FLOWIO_PASSWORD` : mot de passe de cet utilisateur (le script reprend le mot de passe *usine* `flowio1234` ; le remplacer si vous l'avez modifié).
- `POLL_SECONDS` : intervalle de collecte (5 secondes par défaut).
- `DATA_DIR` : emplacement des fichiers générés.

**Sécurité :** ne jamais publier de copie des scripts contenant vos identifiants personnalisés. L'authentification utilise HTTP sur le réseau local ; utiliser ces outils seulement sur un réseau de confiance. Le dossier `flowio-logs/` est ignoré par Git.

## Linux (Debian / Ubuntu)

Depuis la racine du dépôt :

```bash
sudo apt update
sudo apt install -y curl jq util-linux coreutils sed
cd Capture_Flow.io_Logs
chmod 700 collecte_logs_flowio.sh
./collecte_logs_flowio.sh
```

Arrêt au premier plan : `Ctrl+C`. Pour lancer en arrière-plan :

```bash
mkdir -p flowio-logs/control
nohup ./collecte_logs_flowio.sh > flowio-logs/control/collecte-flowio.out 2>&1 &
```

Consulter les journaux : `tail -f flowio-logs/runtime/flowio-logs.log`. Consulter les démarrages : `ls -ltr flowio-logs/boot/`.

Arrêter le collecteur en arrière-plan : `pkill -f '[c]ollecte_logs_flowio.sh'` (vérifier au préalable qu'il s'agit bien du processus souhaité).

**Important :** sous Linux, le chemin `DATA_DIR="./flowio-logs"` est relatif au **répertoire courant**. Exécuter donc le script depuis `Capture_Flow.io_Logs/` comme ci-dessus.

## Windows (PowerShell 5.1 ou 7+)

Aucun programme supplémentaire n'est nécessaire. Depuis la racine du dépôt, ouvrir PowerShell :

```powershell
Set-Location .\Capture_Flow.io_Logs
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\collecte_logs_flowio.ps1
```

Arrêt : `Ctrl+C`. Si Windows a bloqué le fichier téléchargé, vérifier son contenu puis exécuter `Unblock-File .\collecte_logs_flowio.ps1`.

Pour consulter le journal en continu :

```powershell
Get-Content .\flowio-logs\runtime\flowio-logs.log -Tail 30 -Wait
Get-ChildItem .\flowio-logs\boot\
```

Le script PowerShell contient également, en tête de fichier, des exemples de lancement masqué et d'arrêt via le Gestionnaire des tâches/PowerShell.

## Organisation des fichiers créés

```text
Capture_Flow.io_Logs/
├── collecte_logs_flowio.sh
├── collecte_logs_flowio.ps1
├── README.md
└── flowio-logs/                 # créé à l'exécution ; non suivi par Git
    ├── runtime/
    │   └── flowio-logs.log     # flux continu, en ajout
    ├── boot/
    │   └── boot-<boot_id>.log  # une capture par démarrage
    └── control/
        ├── collector.state     # curseur de collecte
        ├── collector.lock      # verrou anti-double exécution
        └── collecte-flowio.out # sortie console si démarré avec nohup
```

Pour une collecte stable, laisser tourner le script pendant les essais et consulter `control/` en cas d'erreur. Les scripts tentent à nouveau les requêtes après une panne réseau. Si la mémoire tampon de logs de Flow.io déborde entre deux collectes, des lignes peuvent être perdues ; un message d'avertissement signale les pertes détectées.
