#!/usr/bin/env bash
# =====================================================================
# COLLECTEUR DE LOGS FLOW.IO - INSTALLATION ET UTILISATION (LINUX)
# =====================================================================
# 1. Installer les dépendances (Debian / Ubuntu) :
#      sudo apt update
#      sudo apt install -y curl jq util-linux coreutils sed
#
# 2. Modifier ci-dessous FLOWIO_IP, FLOWIO_LOGIN et FLOWIO_PASSWORD.
#    Le fichier contient un mot de passe : ne pas le partager publiquement.
#
# 3. Autoriser l'exécution et limiter l'accès au fichier :
#      chmod 700 collecte_logs_flowio.sh
#
# 4. Lancer au premier plan (arrêt avec Ctrl+C) :
#      ./collecte_logs_flowio.sh
#
# 5. Ou lancer en arrière-plan, même après fermeture du terminal :
#      mkdir -p ./flowio-logs/control
#      nohup ./collecte_logs_flowio.sh > ./flowio-logs/control/collecte-flowio.out 2>&1 &
#    Consulter les messages du collecteur :
#      tail -f ./flowio-logs/control/collecte-flowio.out
#    Consulter les logs Flow.io enregistrés :
#      tail -f ./flowio-logs/runtime/flowio-logs.log
#    Consulter les logs BOOT après chaque redémarrage :
#      ls -ltr ./flowio-logs/boot/
#      less ./flowio-logs/boot/boot-*.log
#    Les boots sont archivés séparément, un fichier par boot_id.
#
# Organisation des fichiers (créés automatiquement) :
#      flowio-logs/runtime/flowio-logs.log           logs techniques
#      flowio-logs/boot/boot-<boot_id>.log           logs de chaque boot
#      flowio-logs/control/collector.state          position de lecture
#      flowio-logs/control/collector.lock           verrou anti-doublon
#      flowio-logs/control/collecte-flowio.out      sortie nohup
#    Cette version repart à zéro : aucun ancien fichier n'est migré.
#    Les anciens fichiers éventuels restent en place, sans être touchés.
#
# 6. Arrêter le collecteur lancé avec nohup :
#      pkill -f '[c]ollecte_logs_flowio.sh'
#
# Pré requis : bash, curl, jq, flock (util-linux), sed, coreutils.
# Le script enregistre le journal runtime en ajout, sans l'écraser.
# Les journaux de boot sont téléchargés après chaque démarrage Flow.io,
# dans un fichier distinct (boot-ID.log), et conservés sans écrasement.
# Un boot non encore terminé sera réessayé à la collecte suivante.
# =====================================================================

set -Eeuo pipefail

# =====================================================================
# CONFIGURATION A MODIFIER
# =====================================================================
FLOWIO_IP="192.168.1.50"       # IP ou IP:port du Flow.io (sans http://)
FLOWIO_LOGIN="admin"          # Nom d'utilisateur Flow.io
FLOWIO_PASSWORD="flowio1234"  # Mot de passe par défaut (à changer si modifié sur Flow.io)
POLL_SECONDS=5                # Fréquence d'interrogation en secondes
DATA_DIR="./flowio-logs"     # Dossier parent des 3 sous-dossiers
# =====================================================================

PAGE_SIZE=64                # Maximum autorisé par l'API Flow.io runtime
BOOT_PAGE_SIZE=32           # Pages boot modestes pour préserver la RAM ESP32
MAX_PAGES_PER_CYCLE=8       # Evite de surcharger l'ESP32
BASE_URL="http://${FLOWIO_IP}"
RUNTIME_LOG_DIR="${DATA_DIR}/runtime"
BOOT_LOG_DIR="${DATA_DIR}/boot"
CONTROL_DIR="${DATA_DIR}/control"
LOG_FILE="${RUNTIME_LOG_DIR}/flowio-logs.log"
STATE_FILE="${CONTROL_DIR}/collector.state"
LOCK_FILE="${CONTROL_DIR}/collector.lock"

umask 077
for command in curl jq flock sed date tail mktemp; do
  command -v "$command" >/dev/null 2>&1 || {
    printf 'Erreur : commande manquante : %s\n' "$command" >&2
    exit 1
  }
done

if [[ -z "$FLOWIO_IP" || -z "$FLOWIO_LOGIN" || -z "$FLOWIO_PASSWORD" ]]; then
  echo 'Erreur : renseigner FLOWIO_IP, FLOWIO_LOGIN et FLOWIO_PASSWORD en haut du script.' >&2
  exit 1
fi
if ! [[ "$POLL_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
  echo 'Erreur : POLL_SECONDS doit être un entier positif.' >&2
  exit 1
fi

mkdir -p -- "$RUNTIME_LOG_DIR" "$BOOT_LOG_DIR" "$CONTROL_DIR"
chmod 700 -- "$RUNTIME_LOG_DIR" "$BOOT_LOG_DIR" "$CONTROL_DIR"
touch -- "$LOG_FILE"
chmod 600 -- "$LOG_FILE"
exec 9>"$LOCK_FILE"
chmod 600 -- "$LOCK_FILE"
if ! flock -n 9; then
  echo 'Un autre collecteur utilise déjà ce fichier de logs.' >&2
  exit 1
fi

WORK_DIR="$(mktemp -d "${CONTROL_DIR}/tmp.XXXXXXXX")"
BOOT_TEMP_FILE=""
cleanup() {
  if [[ -n "$BOOT_TEMP_FILE" ]]; then
    rm -f -- "$BOOT_TEMP_FILE"
  fi
  rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT
COOKIE_FILE="${WORK_DIR}/cookies"
RESPONSE_FILE="${WORK_DIR}/response.json"
PASSWORD_FILE="${WORK_DIR}/password"
LINES_FILE="${WORK_DIR}/lines.txt"
printf '%s' "$FLOWIO_PASSWORD" > "$PASSWORD_FILE"
touch "$COOKIE_FILE"

BOOT_ID=""
CURSOR=0
if [[ -s "$STATE_FILE" ]]; then
  read -r BOOT_ID CURSOR < "$STATE_FILE" || true
  if ! [[ "$BOOT_ID" =~ ^[0-9]+$ && "$CURSOR" =~ ^[0-9]+$ ]]; then
    echo "Etat invalide dans $STATE_FILE : reprise à zéro." >&2
    BOOT_ID=""
    CURSOR=0
  fi
fi

now_utc() { date -u '+%Y-%m-%dT%H:%M:%S.%3NZ'; }
notice() {
  local message="$1"
  printf '# [%s] %s\n' "$(now_utc)" "$message" >> "$LOG_FILE"
  printf '%s\n' "$message" >&2
}
save_state() {
  local state_tmp="${STATE_FILE}.tmp.$$"
  printf '%s %s\n' "$BOOT_ID" "$CURSOR" > "$state_tmp"
  mv -f -- "$state_tmp" "$STATE_FILE"
}

login() {
  # Le mot de passe est chargé depuis un fichier privé, et non exposé
  # dans les arguments de curl (visibles par certaines commandes ps).
  if ! curl --silent --show-error --fail --connect-timeout 4 --max-time 15 \
      --cookie "$COOKIE_FILE" --cookie-jar "$COOKIE_FILE" \
      --request POST \
      --data-urlencode "username=${FLOWIO_LOGIN}" \
      --data-urlencode "password@${PASSWORD_FILE}" \
      --output "$RESPONSE_FILE" \
      "${BASE_URL}/api/auth/login"; then
    echo 'Échec de connexion ou authentification à Flow.io.' >&2
    return 1
  fi
  if ! jq -e '.ok == true' "$RESPONSE_FILE" >/dev/null 2>&1; then
    echo 'Réponse de connexion Flow.io invalide.' >&2
    return 1
  fi
}

get_json() {
  local endpoint="$1"
  if curl --silent --show-error --fail --connect-timeout 4 --max-time 15 \
      --cookie "$COOKIE_FILE" --output "$RESPONSE_FILE" \
      "${BASE_URL}${endpoint}"; then
    return 0
  fi
  # La session Flow.io peut expirer : on se reconnecte puis on réessaie.
  echo "Reconnexion Flow.io après échec de ${endpoint%%\?*}..." >&2
  login || return 1
  curl --silent --show-error --fail --connect-timeout 4 --max-time 15 \
    --cookie "$COOKIE_FILE" --output "$RESPONSE_FILE" \
    "${BASE_URL}${endpoint}"
}

# Une archive de boot est immuable une fois complète. Elle ne dépend pas
# du curseur des logs runtime et n'est pas perdue au redémarrage du collecteur.
collect_boot_logs() {
  local id="$1"
  local destination="${BOOT_LOG_DIR}/boot-${id}.log"
  local offset=0 expected=0 dropped=0 state=""
  local current_offset="" entries=0 count=0 end_boot="" now=""

  [[ "$id" =~ ^[0-9]+$ ]] || return 1
  [[ -f "$destination" ]] && return 0

  if ! get_json '/api/logs/boot/status'; then
    echo 'État des logs de boot indisponible ; nouvel essai ultérieur.' >&2
    return 1
  fi
  if ! jq -e '(.state | type == "string") and
      (.entries | type == "number") and (.dropped | type == "number")' \
      "$RESPONSE_FILE" >/dev/null 2>&1; then
    echo 'Réponse /api/logs/boot/status non conforme.' >&2
    return 1
  fi
  state="$(jq -r '.state' "$RESPONSE_FILE")"
  # Les événements de boot ne sont figés qu'à la fin de la capture.
  # Si cette fonctionnalité est absente du firmware, cette étape est ignorée.
  [[ "$state" == "complete" ]] || return 1
  expected="$(jq -r '.entries' "$RESPONSE_FILE")"
  dropped="$(jq -r '.dropped' "$RESPONSE_FILE")"
  if (( expected < 0 || expected > 4096 || dropped < 0 )); then
    echo 'Statistiques de boot incohérentes ; nouvel essai ultérieur.' >&2
    return 1
  fi

  BOOT_TEMP_FILE="${CONTROL_DIR}/boot-${id}.tmp.$$"
  : > "$BOOT_TEMP_FILE"
  chmod 600 -- "$BOOT_TEMP_FILE"
  printf '# [%s] BOOT BEGIN boot_id=%s entries=%s dropped=%s\n' \
    "$(now_utc)" "$id" "$expected" "$dropped" >> "$BOOT_TEMP_FILE"

  while (( offset < expected )); do
    if ! get_json "/api/logs/boot?offset=${offset}&limit=${BOOT_PAGE_SIZE}"; then
      echo "Lecture boot_id=${id} interrompue à offset=${offset}; nouvel essai ultérieur." >&2
      return 1
    fi
    if ! jq -e '(.state == "complete") and (.lines | type == "array") and
        (.offset | type == "number") and (.entries | type == "number") and
        (.count | type == "number") and (.complete | type == "boolean")' \
        "$RESPONSE_FILE" >/dev/null 2>&1; then
      echo "Réponse de page boot invalide (offset=${offset}); nouvel essai ultérieur." >&2
      return 1
    fi
    current_offset="$(jq -r '.offset' "$RESPONSE_FILE")"
    entries="$(jq -r '.entries' "$RESPONSE_FILE")"
    count="$(jq -r '.lines | length' "$RESPONSE_FILE")"
    # Les pages doivent être contiguës; on n'archive jamais un boot incomplet.
    if (( current_offset != offset || entries != expected || count == 0 )); then
      echo "Page boot incohérente (boot=${id} offset=${offset} count=${count})." >&2
      return 1
    fi
    now="$(now_utc)"
    if ! jq -r --arg collected "$now" --arg boot "$id" --argjson offset "$offset" '
      .lines | to_entries[] |
      "[collecte=\($collected)][type=boot][boot=\($boot)][index=\(.key + $offset)] \((.value // "") | gsub("[\r\n]"; "\\n"))"
    ' "$RESPONSE_FILE" | sed -E $'s/\033\[[0-9;]*[[:alpha:]]//g' >> "$BOOT_TEMP_FILE"; then
      echo 'Erreur de formatage de page boot ; nouvel essai ultérieur.' >&2
      return 1
    fi
    (( offset += count ))
    # Enregistre un fichier complet seulement : pas de trous ni de doublons.
    if (( offset > expected )); then
      echo 'La réponse boot excède le nombre attendu.' >&2
      return 1
    fi
  done

  # Protection contre un autre reboot durant la lecture des pages.
  if ! get_json '/api/fwupdate/status'; then
    echo 'Vérification de boot_id impossible ; archive non validée.' >&2
    return 1
  fi
  end_boot="$(jq -er '.boot_id | numbers | tostring' "$RESPONSE_FILE" 2>/dev/null)" || return 1
  if [[ "$end_boot" != "$id" ]]; then
    echo "Un autre reboot est survenu (attendu=${id}, actuel=${end_boot}); archive annulée." >&2
    return 1
  fi

  printf '# [%s] BOOT END boot_id=%s entries=%s dropped=%s\n' \
    "$(now_utc)" "$id" "$offset" "$dropped" >> "$BOOT_TEMP_FILE"
  mv -f -- "$BOOT_TEMP_FILE" "$destination"
  BOOT_TEMP_FILE=""
  notice "Boot archivé : boot_id=${id}, ${offset} entrées, ${dropped} perdues -> ${destination}"
  return 0
}

recover_last_cursor() {
  # Si un arrêt a eu lieu après l'écriture du fichier mais avant la
  # sauvegarde du curseur, récupère le dernier seq réellement archivé.
  local last_line archived_boot archived_seq
  last_line="$(tail -n 1 -- "$LOG_FILE")"
  if [[ "$last_line" =~ \[boot=([0-9]+)\]\[seq=([0-9]+)\] ]]; then
    archived_boot="${BASH_REMATCH[1]}"
    archived_seq="${BASH_REMATCH[2]}"
    if [[ "$archived_boot" == "$BOOT_ID" ]] && (( archived_seq > CURSOR )); then
      CURSOR="$archived_seq"
      save_state
    fi
  fi
}

printf 'Collecte Flow.io : %s -> %s (toutes les %ss)\n' "$FLOWIO_IP" "$LOG_FILE" "$POLL_SECONDS"
login || exit 1

while true; do
  # boot_id change à chaque reboot : les séquences repartent alors à 1.
  if ! get_json '/api/fwupdate/status'; then
    echo 'Statut de démarrage indisponible ; nouvel essai.' >&2
    sleep "$POLL_SECONDS"
    continue
  fi
  current_boot="$(jq -er '.boot_id | numbers | tostring' "$RESPONSE_FILE" 2>/dev/null)" || {
    echo 'Identifiant de démarrage absent de /api/fwupdate/status.' >&2
    sleep "$POLL_SECONDS"
    continue
  }

  if [[ "$BOOT_ID" != "$current_boot" ]]; then
    previous_boot="$BOOT_ID"
    BOOT_ID="$current_boot"
    CURSOR=0
    save_state
    notice "Démarrage Flow.io détecté : boot_id=${BOOT_ID} (précédent=${previous_boot:-aucun})."
  else
    recover_last_cursor
  fi

  for (( page=0; page<MAX_PAGES_PER_CYCLE; page++ )); do
    if ! get_json "/api/logs/runtime?after=${CURSOR}&limit=${PAGE_SIZE}"; then
      echo 'Historique runtime indisponible ; nouvel essai.' >&2
      break
    fi
    if ! jq -e '(.lines | type == "array") and
        (.next | type == "number") and
        (.newest | type == "number") and
        (.oldest | type == "number") and
        (.lost | type == "number")' "$RESPONSE_FILE" >/dev/null 2>&1; then
      echo 'Réponse /api/logs/runtime non conforme ; curseur conservé.' >&2
      break
    fi

    newest="$(jq -r '.newest' "$RESPONSE_FILE")"
    oldest="$(jq -r '.oldest' "$RESPONSE_FILE")"
    next="$(jq -r '.next' "$RESPONSE_FILE")"
    lost="$(jq -r '.lost' "$RESPONSE_FILE")"
    complete="$(jq -r '.complete' "$RESPONSE_FILE")"
    count="$(jq -r '.lines | length' "$RESPONSE_FILE")"

    if (( newest < CURSOR )); then
      # Protection supplémentaire si les compteurs reviennent à zéro.
      notice "Séquence réinitialisée (dernier=${CURSOR}, nouveau=${newest}); redémarrage de lecture."
      CURSOR=0
      save_state
      break
    fi
    if (( lost > 0 )); then
      notice "ATTENTION : ${lost} logs effacés de la PSRAM avant collecte (oldest=${oldest})."
    fi
    if (( count > 0 )); then
      captured_at="$(now_utc)"
      # Une date de collecte + boot + séquence par entrée. La ligne originale
      # contient aussi la date Flow.io, si son horloge était synchronisée.
      # Suppression des codes ANSI de couleur et des retours ligne embarqués.
      if ! jq -r --arg collected "$captured_at" --arg boot "$BOOT_ID" '
        .lines[] | "[collecte=\($collected)][boot=\($boot)][seq=\(.seq)] \((.line // "") | gsub("[\\r\\n]"; "\\\\n"))"
      ' "$RESPONSE_FILE" | sed -E $'s/\033\[[0-9;]*[[:alpha:]]//g' > "$LINES_FILE"; then
        echo 'Erreur de formatage JSON ; curseur conservé.' >&2
        break
      fi
      cat -- "$LINES_FILE" >> "$LOG_FILE"
    fi

    if (( next > CURSOR )); then
      CURSOR="$next"
      save_state
    else
      break
    fi
    if [[ "$complete" == "true" || "$count" -lt "$PAGE_SIZE" ]]; then
      break
    fi
  done

  # La PSRAM boot est figée après démarrage. On la récupère dès qu'elle
  # est prête, même après un redémarrage du collecteur Linux.
  # Une archive déjà téléchargée ne sera jamais récupérée en double.
  collect_boot_logs "$BOOT_ID" || true
  sleep "$POLL_SECONDS"
done