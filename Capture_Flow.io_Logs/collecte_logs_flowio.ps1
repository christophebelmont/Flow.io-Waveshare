#requires -Version 5.1
<#
========================================================================
 COLLECTEUR DE LOGS FLOW.IO - WINDOWS POWERSHELL 5.1 / POWERSHELL 7+
========================================================================
 1. Enregistrer ce fichier sous : collecte_logs_flowio.ps1
    Modifier FLOWIO_IP, FLOWIO_LOGIN et FLOWIO_PASSWORD ci-dessous.
    Attention : le mot de passe figure en clair dans ce script. Ne pas
    partager le fichier et limiter ses autorisations d'acces Windows.

 2. Aucune dependance externe : PowerShell et Windows suffisent.
    En cas de blocage d'un fichier telecharge :
      Unblock-File .\collecte_logs_flowio.ps1

 3. Demarrer dans une console PowerShell :
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\collecte_logs_flowio.ps1
    Arret au premier plan : Ctrl+C.

 4. Demarrer en arriere-plan (depuis le dossier du script) :
      $scriptPath = (Resolve-Path .\collecte_logs_flowio.ps1).Path
      $control = Join-Path (Split-Path $scriptPath -Parent) 'flowio-logs\control'
      New-Item -ItemType Directory -Force -Path $control | Out-Null
      Start-Process powershell.exe -WindowStyle Hidden `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`"" `
        -RedirectStandardOutput (Join-Path $control 'collecte-flowio.out') `
        -RedirectStandardError (Join-Path $control 'collecte-flowio.err')
    Pour un demarrage meme sans session Windows ouverte, utiliser le
    Planificateur de taches (action : powershell.exe -NoProfile
    -ExecutionPolicy Bypass -File "C:\chemin\collecte_logs_flowio.ps1").

 5. Voir les logs et les erreurs du collecteur :
      Get-Content .\flowio-logs\runtime\flowio-logs.log -Tail 30 -Wait
      Get-Content .\flowio-logs\control\collecte-flowio.err -Tail 30 -Wait
      Get-ChildItem .\flowio-logs\boot\

 6. Arreter le processus en arriere-plan :
      Get-CimInstance Win32_Process |
        Where-Object { $_.Name -eq 'powershell.exe' -and
          $_.CommandLine -match 'collecte_logs_flowio\.ps1' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId }

 Fichiers crees automatiquement a cote du script :
      flowio-logs\runtime\flowio-logs.log       : logs runtime continus
      flowio-logs\boot\boot-<boot_id>.log       : un fichier par boot
      flowio-logs\control\collector.state      : curseur de lecture
      flowio-logs\control\collector.lock       : protection double lancement
      flowio-logs\control\collecte-flowio.out  : sortie de Start-Process
      flowio-logs\control\collecte-flowio.err  : erreurs de Start-Process

 Le fichier runtime est complete sans etre ecrase. Chaque ligne garde
 l'horodatage Flow.io et ajoute celui de la collecte Windows (UTC).
 Le script detecte les reboots via /api/fwupdate/status, recharge les
 logs boot une fois la capture terminee, et restaure les curseurs.
 Les anciens fichiers d'une installation Linux ne sont pas modifies.
========================================================================
#>

# =======================================================================
# CONFIGURATION UTILISATEUR
# =======================================================================
$FLOWIO_IP       = '192.168.1.50'   # Adresse IPv4 ou IPv4:port, sans http://
$FLOWIO_LOGIN    = 'admin'
$FLOWIO_PASSWORD = 'flowio1234'     # Mot de passe Flow.io par defaut
$POLL_SECONDS    = 5                # Intervalle de lecture, en secondes
$DATA_DIR        = Join-Path $PSScriptRoot 'flowio-logs'
# =======================================================================

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PAGE_SIZE = 64
$BOOT_PAGE_SIZE = 32
$MAX_PAGES_PER_CYCLE = 8
$BASE_URL = 'http://' + $FLOWIO_IP.TrimEnd('/')

$RuntimeDir = Join-Path $DATA_DIR 'runtime'
$BootDir = Join-Path $DATA_DIR 'boot'
$ControlDir = Join-Path $DATA_DIR 'control'
$LogFile = Join-Path $RuntimeDir 'flowio-logs.log'
$StateFile = Join-Path $ControlDir 'collector.state'
$LockFile = Join-Path $ControlDir 'collector.lock'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

$script:Session = $null
$script:BootId = ''
$script:Cursor = [long]0
$script:LockHandle = $null

function Now-Utc {
    return [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ',
        [System.Globalization.CultureInfo]::InvariantCulture)
}

function Append-Utf8([string]$Path, [string]$Text) {
    [System.IO.File]::AppendAllText($Path, $Text, $Utf8NoBom)
}

function Notice([string]$Text) {
    Append-Utf8 $LogFile ("# [collecte=$(Now-Utc)] $Text" + [Environment]::NewLine)
    [Console]::Error.WriteLine($Text)
}

function Save-State {
    # Fichier temporaire + remplacement atomique sur le meme volume.
    $temp = Join-Path $ControlDir ('collector.state.' + [guid]::NewGuid().ToString('N') + '.tmp')
    $json = @{ boot_id = $script:BootId; cursor = $script:Cursor } | ConvertTo-Json -Compress
    try {
        [System.IO.File]::WriteAllText($temp, $json + [Environment]::NewLine, $Utf8NoBom)
        if ([System.IO.File]::Exists($StateFile)) {
            [System.IO.File]::Replace($temp, $StateFile, $null)
        } else {
            [System.IO.File]::Move($temp, $StateFile)
        }
    } finally {
        if ([System.IO.File]::Exists($temp)) {
            [System.IO.File]::Delete($temp)
        }
    }
}

function Load-State {
    if (-not [System.IO.File]::Exists($StateFile)) { return }
    try {
        $old = [System.IO.File]::ReadAllText($StateFile) | ConvertFrom-Json
        $oldId = [string]$old.boot_id
        $oldCursor = [long]$old.cursor
        if ($oldId -notmatch '^\d+$' -or $oldCursor -lt 0) {
            throw 'Identifiant ou curseur invalide'
        }
        $script:BootId = $oldId
        $script:Cursor = $oldCursor
    } catch {
        [Console]::Error.WriteLine('Etat precedent invalide : reprise au prochain boot_id detecte.')
        $script:BootId = ''
        $script:Cursor = [long]0
    }
}

function Recover-Last-Cursor {
    # Reparer le cas : ligne deja ecrite mais interruption avant Save-State.
    if ($script:BootId -eq '' -or -not [System.IO.File]::Exists($LogFile)) { return }
    foreach ($line in @(Get-Content -LiteralPath $LogFile -Tail 512 -ErrorAction SilentlyContinue)) {
        if ($line -match '^\[collecte=.*?\]\[boot=(\d+)\]\[seq=(\d+)\] ') {
            if ($Matches[1] -eq $script:BootId) {
                $seq = [long]$Matches[2]
                if ($seq -gt $script:Cursor) { $script:Cursor = $seq }
            }
        }
    }
    Save-State
}

function Login-Flowio {
    $script:Session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $response = Invoke-RestMethod -Uri ($BASE_URL + '/api/auth/login') `
        -Method Post -WebSession $script:Session -ContentType 'application/x-www-form-urlencoded' `
        -Body @{ username = $FLOWIO_LOGIN; password = $FLOWIO_PASSWORD } `
        -TimeoutSec 15 -UseBasicParsing -ErrorAction Stop
    if ($null -eq $response -or $response.ok -ne $true) {
        throw 'Connexion Flow.io refusee (verifier login / mot de passe).'
    }
}

function Get-FlowioJson([string]$Endpoint) {
    if ($null -eq $script:Session) { Login-Flowio }
    $uri = $BASE_URL + $Endpoint
    for ($attempt = 0; $attempt -lt 2; $attempt++) {
        try {
            $response = Invoke-RestMethod -Uri $uri -Method Get `
                -WebSession $script:Session -TimeoutSec 15 -UseBasicParsing -ErrorAction Stop
            if ($null -eq $response) { throw 'Reponse HTTP vide' }
            return $response
        } catch {
            if ($attempt -eq 1) { throw }
            # Reauthentification si session expiree ou apres panne reseau.
            Login-Flowio
        }
    }
}

function Get-Current-BootId {
    $status = Get-FlowioJson '/api/fwupdate/status'
    $id = [string]$status.boot_id
    if ($id -notmatch '^\d+$') { throw 'boot_id absent de /api/fwupdate/status' }
    return $id
}

function Clean-LogLine([object]$Value) {
    $line = [string]$Value
    # Enlever les couleurs ANSI et echapper les retours-ligne embarques.
    $line = [regex]::Replace($line, "\x1B\[[0-9;]*[A-Za-z]", '')
    return $line.Replace("`r", '\r').Replace("`n", '\n')
}

function Collect-Runtime {
    for ($pageIndex = 0; $pageIndex -lt $MAX_PAGES_PER_CYCLE; $pageIndex++) {
        $page = Get-FlowioJson ("/api/logs/runtime?after=$($script:Cursor)&limit=$PAGE_SIZE")
        if ($null -eq $page.lines -or $null -eq $page.next -or
            $null -eq $page.newest -or $null -eq $page.oldest -or $null -eq $page.lost) {
            throw 'Format de /api/logs/runtime inattendu : curseur inchange.'
        }
        $newest = [long]$page.newest
        $next = [long]$page.next
        $lost = [long]$page.lost
        $oldest = [long]$page.oldest
        $entries = @($page.lines)

        if ($newest -lt $script:Cursor) {
            Notice "Sequence runtime reinitialisee (ancien=$($script:Cursor), newest=$newest)."
            $script:Cursor = 0
            Save-State
            return
        }
        if ($lost -gt 0) {
            Notice "ATTENTION : $lost logs perdus (historique PSRAM ecrase, oldest=$oldest)."
        }
        if ($entries.Count -gt 0) {
            # Confirmer qu'aucun reboot n'a eu lieu pendant la lecture HTTP.
            if ((Get-Current-BootId) -ne $script:BootId) {
                Notice 'Reboot pendant la collecte runtime : page abandonnee, nouvelle detection au cycle suivant.'
                return
            }
            $collected = Now-Utc
            $lines = New-Object 'System.Collections.Generic.List[string]'
            $previousSeq = [long]$script:Cursor
            foreach ($entry in $entries) {
                if ($null -eq $entry.seq -or $null -eq $entry.line) {
                    throw 'Ligne runtime non conforme : abandon de la page.'
                }
                $seq = [long]$entry.seq
                if ($seq -le $previousSeq) {
                    throw "Sequences runtime non monotones (seq=$seq, preced=$previousSeq)."
                }
                $previousSeq = $seq
                $text = Clean-LogLine $entry.line
                $lines.Add("[collecte=$collected][boot=$($script:BootId)][seq=$seq] $text")
            }
            if ($next -lt $previousSeq) {
                throw "Curseur runtime incoherent (next=$next, seq=$previousSeq)."
            }
            Append-Utf8 $LogFile (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
        }
        if ($next -le $script:Cursor) { return }
        $script:Cursor = $next
        Save-State
        if ($page.complete -eq $true -or $entries.Count -lt $PAGE_SIZE) { return }
    }
}

function Collect-Boot([string]$Id) {
    if ($Id -notmatch '^\d+$') { return }
    $destination = Join-Path $BootDir ("boot-$Id.log")
    if ([System.IO.File]::Exists($destination)) { return }

    $status = Get-FlowioJson '/api/logs/boot/status'
    if ($status.state -ne 'complete') { return } # Boot pas encore fige ou non disponible.
    $expected = [int]$status.entries
    $dropped = [long]$status.dropped
    if ($expected -lt 0 -or $expected -gt 4096 -or $dropped -lt 0) {
        throw 'Statistiques de boot invalides.'
    }

    $temp = Join-Path $ControlDir ("boot-$Id." + [guid]::NewGuid().ToString('N') + '.tmp')
    $valid = $false
    try {
        [System.IO.File]::WriteAllText($temp,
            "# [$(Now-Utc)] BOOT BEGIN boot_id=$Id entries=$expected dropped=$dropped" + [Environment]::NewLine,
            $Utf8NoBom)
        $offset = 0
        while ($offset -lt $expected) {
            $page = Get-FlowioJson ("/api/logs/boot?offset=$offset&limit=$BOOT_PAGE_SIZE")
            if ($page.state -ne 'complete' -or [int]$page.offset -ne $offset -or
                [int]$page.entries -ne $expected -or $null -eq $page.lines) {
                throw "Page de boot incoherente (boot=$Id offset=$offset)."
            }
            $entries = @($page.lines)
            if ($entries.Count -le 0 -or $offset + $entries.Count -gt $expected) {
                throw "Page de boot incomplete (boot=$Id offset=$offset)."
            }
            $at = Now-Utc
            $lines = New-Object 'System.Collections.Generic.List[string]'
            for ($i = 0; $i -lt $entries.Count; $i++) {
                $cleaned = Clean-LogLine $entries[$i]
                $index = $offset + $i
                $lines.Add("[collecte=$at][type=boot][boot=$Id][index=$index] $cleaned")
            }
            Append-Utf8 $temp (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
            $offset += $entries.Count
        }
        # Ne pas archiver un mélange de deux boots si reboot durant les pages.
        if ((Get-Current-BootId) -ne $Id) {
            throw 'Reboot pendant le chargement : archive de boot abandonnee.'
        }
        Append-Utf8 $temp ("# [$(Now-Utc)] BOOT END boot_id=$Id entries=$offset dropped=$dropped" + [Environment]::NewLine)
        [System.IO.File]::Move($temp, $destination)
        $valid = $true
    } finally {
        if (-not $valid -and [System.IO.File]::Exists($temp)) {
            [System.IO.File]::Delete($temp)
        }
    }
    Notice "Boot archive : boot_id=$Id, $expected entrees, $dropped perdues -> $destination"
}

if ([string]::IsNullOrWhiteSpace($FLOWIO_IP) -or
    [string]::IsNullOrWhiteSpace($FLOWIO_LOGIN) -or
    [string]::IsNullOrWhiteSpace($FLOWIO_PASSWORD)) {
    throw 'Renseigner FLOWIO_IP, FLOWIO_LOGIN et FLOWIO_PASSWORD.'
}
if ($POLL_SECONDS -lt 1) { throw 'POLL_SECONDS doit etre au moins egal a 1.' }

foreach ($dir in @($RuntimeDir, $BootDir, $ControlDir)) {
    if (-not [System.IO.Directory]::Exists($dir)) {
        [void][System.IO.Directory]::CreateDirectory($dir)
    }
}
if (-not [System.IO.File]::Exists($LogFile)) {
    [System.IO.File]::WriteAllText($LogFile, '', $Utf8NoBom)
}

try {
    # Ouvrir le fichier sans partage : un seul collecteur pour ce repertoire.
    $script:LockHandle = [System.IO.File]::Open(
        $LockFile, [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
} catch {
    throw 'Un autre collecteur est deja lance (collector.lock est verrouille).'
}

try {
    Load-State
    Recover-Last-Cursor
    [Console]::WriteLine("Collecte Flow.io : $FLOWIO_IP -> $LogFile, toutes les ${POLL_SECONDS}s")
    while ($true) {
        try {
            $currentId = Get-Current-BootId
            if ($script:BootId -ne $currentId) {
                $previous = $script:BootId
                $script:BootId = $currentId
                $script:Cursor = [long]0
                Save-State
                if ($previous -eq '') { $previous = 'aucun' }
                Notice "Boot Flow.io detecte : boot_id=$currentId (precedent=$previous)."
            }
            Collect-Runtime
            # Recuperer la capture PSRAM une fois son etat 'complete'.
            Collect-Boot $currentId
        } catch {
            [Console]::Error.WriteLine("[$(Now-Utc)] Collecte interrompue pour ce cycle : $($_.Exception.Message)")
            # Auth/session reinitialisees lors de la prochaine tentative.
            try { Login-Flowio } catch {
                [Console]::Error.WriteLine("[$(Now-Utc)] Reconnexion impossible : $($_.Exception.Message)")
            }
        }
        Start-Sleep -Seconds $POLL_SECONDS
    }
} finally {
    if ($null -ne $script:LockHandle) {
        $script:LockHandle.Dispose()
    }
}