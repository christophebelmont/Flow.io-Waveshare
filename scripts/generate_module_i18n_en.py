#!/usr/bin/env python3
"""Generate module `text/i18n.en.json` files from `text/i18n.fr.json`.

The translator is deterministic (domain glossary + regex templates), so it can
run locally without network dependency.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Dict, Iterable, Tuple


PROJECT_ROOT = Path(__file__).resolve().parents[1]
MODULES_DIR = PROJECT_ROOT / "src" / "Modules"


_EXACT_MAP: Dict[str, str] = {
    "Actif": "Active",
    "Act.": "Act.",
    "Arret": "Off",
    "Arrêt": "Off",
    "Aucun": "None",
    "Connecte": "Connected",
    "Deconnecte": "Disconnected",
    "Alarmes": "Alarms",
    "Alarmes actives": "Active alarms",
    "Alarmes rearmables": "Resettable alarms",
    "Dashboard": "Dashboard",
    "Debug": "Debug",
    "Info": "Info",
    "Warning": "Warning",
    "Error": "Error",
    "Firmware": "Firmware",
    "Adresse IP": "IP address",
    "Port": "Port",
    "Port MQTT": "MQTT port",
    "Utilisateur MQTT": "MQTT username",
    "Mot de passe MQTT": "MQTT password",
    "ID client MQTT": "MQTT client ID",
    "Topic base": "Base topic",
    "flow.io": "flow.io",
    "Francais (fr)": "French (fr)",
    "English (en)": "English (en)",
    "Sondes": "Sensors",
    "Mode": "Mode",
    "Paramètres": "Settings",
    "Configuration": "Configuration",
    "Blob planning": "Schedule blob",
    "Blob métriques": "Metrics blob",
    "Couleur de fond": "Background color",
    "Cond.": "Cond.",
    "Niveau": "Level",
    "Langue": "Language",
    "Constructeur": "Manufacturer",
    "Adresse locale flow.io": "flow.io local address",
    "Adresse cible flow.io": "flow.io target address",
    "Client eLink actif": "eLink client enabled",
    "Activation appareil": "Device enabled",
    "État": "State",
    "Etat": "State",
    "Actif a 1 I00": "Active-high I00",
    "Actif a 1 I01": "Active-high I01",
    "Actif a 1 I02": "Active-high I02",
    "Actif a 1 I03": "Active-high I03",
    "Actif a 1 I04": "Active-high I04",
    "Actif a 1 D00": "Active-high D00",
    "Actif a 1 D01": "Active-high D01",
    "Actif a 1 D02": "Active-high D02",
    "Actif a 1 D03": "Active-high D03",
    "Actif a 1 D04": "Active-high D04",
    "Actif a 1 D05": "Active-high D05",
    "Actif a 1 D06": "Active-high D06",
    "Actif a 1 D07": "Active-high D07",
    "Un Front [1]": "Single edge [1]",
    "Deux Fronts [2]": "Both edges [2]",
    "Montant [3]": "Rising edge [3]",
    "Descendant [4]": "Falling edge [4]",
    "ADS Interne": "Internal ADS",
    "ADS Externe": "External ADS",
    "Chlorine Pump [2]": "Chlorine Pump [2]",
    "Chlorine Generator [5]": "Chlorine Generator [5]",
}


_REPLACEMENTS: Tuple[Tuple[str, str], ...] = (
    ("éte", "DST"),
    ("d'été", "DST"),
    ("Amerique du Nord", "North America"),
    ("Amerique du Sud", "South America"),
    ("Atlantique", "Atlantic"),
    ("Australie", "Australia"),
    ("Asie du Sud-Est", "Southeast Asia"),
    ("Asie", "Asia"),
    ("Chine", "China"),
    ("Bangladesh / Bhoutan", "Bangladesh / Bhutan"),
    ("Acores", "Azores"),
    ("nord-ouest", "northwest"),
    ("sud-ouest", "southwest"),
    ("sud-est", "southeast"),
    ("nord-est", "northeast"),
    (" ouest ", " west "),
    (" sud ", " south "),
    ("montagnes", "mountain"),
    ("centre", "central"),
    ("du Nord", "of North"),
    ("du Sud", "of South"),
    (" / ete ", " / DST "),
    ("Choix de la valeur runtime flow.io a demander via eLink pour ce slot.", "Choose the flow.io runtime value to request via eLink for this slot."),
    ("Chemin de base des mises à jour sur le serveur HTTP, concaténé avec les chemins des images.", "Base path for updates on the HTTP server, concatenated with image paths."),
    ("Chemin du binaire firmware flow.io sur le serveur MAJ.", "Path to the flow.io firmware binary on the update server."),
    ("Chemin du fichier TFT Nextion sur le serveur MAJ.", "Path to the Nextion TFT file on the update server."),
    ("Chemin du fichier spiffs.bin sur le serveur MAJ.", "Path to the spiffs.bin file on the update server."),
    ("Adresse du broker MQTT (DNS ou IP).", "MQTT broker address (DNS or IP)."),
    ("Adresse du serveur HTTP hébergeant les firmwares.", "Address of the HTTP server hosting firmware files."),
    ("Active l'émission periodique des traces IO.", "Enable periodic IO trace emission."),
    ("Active l'emission periodique de la temperature d'eau vers un afficheur TFA Venice compatible.", "Enable periodic water temperature transmission to a compatible TFA Venice display."),
    ("Active le driver", "Enable"),
    ("Active la logique", "Enable"),
    ("Active la régulation", "Enable"),
    ("Active la surveillance", "Enable"),
    ("Active le client", "Enable"),
    ("Active le serveur", "Enable"),
    ("Active ou désactive", "Enable or disable"),
    ("Active ou masque", "Enable or hide"),
    ("Active un fonctionnement impulsionnel", "Enable pulse mode"),
    ("Autorise le reboot automatique", "Allow automatic reboot"),
    ("Autorise le HMIModule", "Allow HMIModule"),
    ("Adresse I2C du capteur", "I2C address of sensor"),
    ("Adresse I2C du composant", "I2C address of component"),
    ("Adresse I2C du convertisseur", "I2C address of converter"),
    ("Adresse I2C du serveur de configuration", "I2C address of configuration server"),
    ("Adresse I2C locale du serveur de configuration", "Local I2C address of configuration server"),
    ("Broche SCL du bus I2C utilisé par le module IO.", "SCL pin of the I2C bus used by IO module."),
    ("Broche SDA du bus I2C utilisé par le module IO.", "SDA pin of the I2C bus used by IO module."),
    ("Intervalle entre deux", "Interval between two"),
    ("Intervalle de rafraîchissement", "Refresh interval"),
    ("Période", "Period"),
    ("Fréquence", "Frequency"),
    ("Fréquence d'échantillonnage", "Sampling frequency"),
    ("Gain de conversion", "Conversion gain"),
    ("adresse", "address"),
    ("Adresse", "Address"),
    ("Actif", "Enabled"),
    ("actif", "enabled"),
    ("Nom lisible", "Readable name"),
    ("Nom entrée", "Input name"),
    ("Nom sortie", "Output name"),
    ("Nombre de décimales conservées pour", "Number of decimals kept for"),
    ("Precision", "Precision"),
    ("Port physique", "Physical port"),
    ("Identifiant du port physique utilise", "Identifier of physical port used"),
    ("Identifiant du port physique pilote par", "Identifier of physical port driven by"),
    ("par l'entree digitale", "by digital input"),
    ("par l'entrée digitale", "by digital input"),
    ("La valeur reference un binding compile-time autorise, pas un numero de GPIO brut.", "Value references an allowed compile-time binding, not a raw GPIO number."),
    ("Coefficient multiplicateur de calibration", "Calibration multiplier coefficient"),
    ("Offset de calibration", "Calibration offset"),
    ("Il ajuste l'echelle de la mesure brute avant publication.", "Adjusts raw measurement scale before publish."),
    ("Il ajoute un decalage a la mesure convertie pour corriger le zero ou appliquer une translation.", "Adds an offset to converted measurement to correct zero or apply translation."),
    ("Formule: value = c0 * input + c1.", "Formula: value = c0 * input + c1."),
    ("Coefficient multiplicateur applique au nombre d'impulsions", "Multiplier coefficient applied to pulse count"),
    ("en mode compteur.", "in counter mode."),
    ("Il convertit les impulsions brutes en valeur cumulee publiee.", "Converts raw pulses to published cumulative value."),
    ("Formule: value = c0 * impulsions.", "Formula: value = c0 * pulses."),
    ("Valeur cumulée éditable pour", "Editable cumulative value for"),
    ("Cette correction écrase immédiatement le total actuel du compteur, devient la nouvelle statistique long terme et est écrite immédiatement en NVS.", "This correction immediately overwrites the current counter total, becomes the new long-term statistic, and is written to NVS immediately."),
    ("Front a compter pour", "Edge to count for"),
    ("(0=descendant, 1=montant, 2=les deux).", "(0=falling, 1=rising, 2=both)."),
    ("Si active, l'input", "If enabled, input"),
    ("est consideree active a l'etat haut.", "is considered active in high state."),
    ("Si vrai,", "If true,"),
    ("active à l'état haut; sinon active-bas.", "is active-high; otherwise active-low."),
    ("Choisit le type de fonctionnement de", "Select operating mode for"),
    ("Changement pris en compte au restart.", "Change applied on restart."),
    ("Serveur utilisé for synchronisation de l'heure.", "Server used for time synchronization."),
    ("Définit le premier jour de semaine fors plannings.", "Defines first day of week for schedules."),
    ("Configuration sérialisée des slots du scheduler.", "Serialized scheduler slots configuration."),
    ("Masque de dépendances inter-appareils.", "Inter-device dependency mask."),
    ("Temps maximal autorisé par jour pour l'appareil.", "Maximum allowed runtime per day for this device."),
    ("Volume initial de cuve après configuration/reset.", "Initial tank volume after configuration/reset."),
    ("Cumul compteur", "Counter total"),
    ("Capacité cuve (mL)", "Tank capacity (mL)"),
    ("Capacité maximale de la cuve associée à l'appareil.", "Maximum capacity of the tank associated with this device."),
    ("Durée", "Duration"),
    ("Début", "Start"),
    ("Arrêt", "Stop"),
    ("Consigne", "Setpoint"),
    ("Température", "Temperature"),
    ("Pression", "Pressure"),
    ("Mode auto", "Auto mode"),
    ("Filtration", "Filtration"),
    ("Hivernage", "Winter mode"),
    ("Système", "System"),
    ("Sonde", "Sensor"),
    ("système", "system"),
    ("surveillance", "monitoring"),
    ("connexion WiFi", "WiFi connection"),
    ("synchronisation horaire NTP", "NTP time synchronization"),
    ("mises a jour firmware", "firmware updates"),
    ("Niveau minimal de logs", "Minimum log level"),
    ("pour le module", "for module"),
    ("pour la", "for"),
    ("pour le", "for"),
    ("pour les", "for"),
    ("échecs consécutifs", "consecutive failures"),
    ("échecs max", "max failures"),
    ("grâce", "grace"),
    ("seuil blocage", "stall threshold"),
)


_RE_TIME = re.compile(r"^\d{2}:\d{2}$")
_RE_HEX = re.compile(r"^#[0-9A-Fa-f]{6}$")
_RE_BOOL_ENUM = re.compile(r"^(\d+)\s+\|\s+(.+)$")
_RE_C0 = re.compile(r"^Coefficient C0 ([AI]\d{2})$")
_RE_C1 = re.compile(r"^Offset C1 ([AI]\d{2})$")
_RE_INPUT_NAME = re.compile(r"^Nom entrée ([AI]\d{2})$")
_RE_OUTPUT_NAME = re.compile(r"^Nom sortie (D\d{2})$")
_RE_PREC = re.compile(r"^Precision ([AI]\d{2})$")
_RE_BIND = re.compile(r"^Port physique ([ADI]\d{2})$")
_RE_DURATION_OUT = re.compile(r"^Durée impulsion (D\d{2}) \(ms\)$")
_WORD_REPLACEMENTS: Tuple[Tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"\bpour\b", re.IGNORECASE), "for"),
    (re.compile(r"\bavec\b", re.IGNORECASE), "with"),
    (re.compile(r"\bsans\b", re.IGNORECASE), "without"),
    (re.compile(r"\bentre\b", re.IGNORECASE), "between"),
    (re.compile(r"\bdeux\b", re.IGNORECASE), "two"),
    (re.compile(r"\bune\b", re.IGNORECASE), "a"),
    (re.compile(r"\bun\b", re.IGNORECASE), "one"),
    (re.compile(r"\bles\b", re.IGNORECASE), "the"),
    (re.compile(r"\ble\b", re.IGNORECASE), "the"),
    (re.compile(r"\bla\b", re.IGNORECASE), "the"),
    (re.compile(r"\bdes\b", re.IGNORECASE), "of"),
    (re.compile(r"\bdu\b", re.IGNORECASE), "of"),
    (re.compile(r"\bcompteur\b", re.IGNORECASE), "counter"),
    (re.compile(r"\bimpulsion\b", re.IGNORECASE), "pulse"),
    (re.compile(r"\bimpulsions\b", re.IGNORECASE), "pulses"),
    (re.compile(r"\blectures\b", re.IGNORECASE), "reads"),
    (re.compile(r"\blecture\b", re.IGNORECASE), "read"),
    (re.compile(r"\bmesure\b", re.IGNORECASE), "measurement"),
    (re.compile(r"\bmesures\b", re.IGNORECASE), "measurements"),
    (re.compile(r"\bsortie\b", re.IGNORECASE), "output"),
    (re.compile(r"\bsorties\b", re.IGNORECASE), "outputs"),
    (re.compile(r"\bentrée\b", re.IGNORECASE), "input"),
    (re.compile(r"\bentree\b", re.IGNORECASE), "input"),
    (re.compile(r"\bnum[eé]ro\b", re.IGNORECASE), "number"),
    (re.compile(r"\br[eé]f[eé]rence\b", re.IGNORECASE), "reference"),
    (re.compile(r"\bdur[eé]e\b", re.IGNORECASE), "duration"),
    (re.compile(r"\bvaleur\b", re.IGNORECASE), "value"),
    (re.compile(r"\bvaleurs\b", re.IGNORECASE), "values"),
)


def _is_non_translatable(value: str) -> bool:
    return bool(
        _RE_HEX.match(value)
        or _RE_TIME.match(value)
        or value.startswith("UTC")
        or value.startswith("#")
    )


def _apply_templates(value: str) -> str:
    m = _RE_BOOL_ENUM.match(value)
    if m:
        idx, txt = m.group(1), m.group(2).strip()
        if txt == "Etat":
            return f"{idx} | State"
        if txt == "Compteur d'Impulsion":
            return f"{idx} | Pulse Counter"
    m = _RE_C0.match(value)
    if m:
        return f"C0 Coefficient {m.group(1)}"
    m = _RE_C1.match(value)
    if m:
        return f"C1 Offset {m.group(1)}"
    m = _RE_INPUT_NAME.match(value)
    if m:
        return f"Input name {m.group(1)}"
    m = _RE_OUTPUT_NAME.match(value)
    if m:
        return f"Output name {m.group(1)}"
    m = _RE_PREC.match(value)
    if m:
        return f"Precision {m.group(1)}"
    m = _RE_BIND.match(value)
    if m:
        return f"Physical port {m.group(1)}"
    m = _RE_DURATION_OUT.match(value)
    if m:
        return f"Pulse duration {m.group(1)} (ms)"
    return value


def _normalize_spacing(value: str) -> str:
    out = value.replace("  ", " ")
    out = out.replace(" .", ".")
    out = out.replace(" ,", ",")
    out = out.replace(" ;", ";")
    out = out.replace(" :", ":")
    out = out.replace(" d'", " of ")
    out = out.replace(" l'", " ")
    out = out.replace("  ", " ")
    return out.strip()


def translate(value: str) -> str:
    if not isinstance(value, str):
        return value
    src = value.strip()
    if not src:
        return src
    if _is_non_translatable(src):
        return src
    if src in _EXACT_MAP:
        return _EXACT_MAP[src]

    out = _apply_templates(src)
    if out != src and out in _EXACT_MAP:
        out = _EXACT_MAP[out]

    for fr, en in _REPLACEMENTS:
        out = out.replace(fr, en)

    out = out.replace("é", "e") if "é" in out and "Enable" in out and "Période" not in src else out
    out = out.replace("démarrage", "startup")
    out = out.replace("redemarrage", "restart")
    out = out.replace("réseau", "network")
    out = out.replace("réseaux", "networks")
    out = out.replace("réponse", "response")
    out = out.replace("indisponible", "unavailable")
    out = out.replace("disponible", "available")
    out = out.replace("configurée", "configured")
    out = out.replace("configurer", "configure")
    out = out.replace("configuration", "configuration")
    out = out.replace("défaut", "default")
    out = out.replace("définit", "defines")
    out = out.replace("délai", "delay")
    out = out.replace("écran", "screen")
    out = out.replace("évaluation", "evaluation")
    out = out.replace("électrolyse", "electrolysis")
    out = out.replace("piscine", "pool")
    out = out.replace("chauffage", "heating")
    out = out.replace("consigne", "setpoint")
    out = out.replace("température", "temperature")
    out = out.replace("pression", "pressure")
    out = out.replace("entrée", "input")
    out = out.replace("sortie", "output")
    out = out.replace("durée", "duration")
    out = out.replace("débit", "flow rate")
    out = out.replace("périodique", "periodic")
    out = out.replace("paramètres", "settings")
    out = out.replace("automatique", "automatic")
    out = out.replace("désactive", "disable")
    out = out.replace("désactivée", "disabled")
    out = out.replace("activée", "enabled")
    out = out.replace("activé", "enabled")
    out = out.replace("relais", "relay")
    out = out.replace("pompe", "pump")
    out = out.replace("alarme", "alarm")
    out = out.replace("alarmes", "alarms")
    out = out.replace("serveur", "server")
    out = out.replace("client", "client")
    out = out.replace("panneau", "panel")
    out = out.replace("et est", "and is")

    for pattern, repl in _WORD_REPLACEMENTS:
        out = pattern.sub(repl, out)

    return _normalize_spacing(out)


def _iter_fr_files() -> Iterable[Path]:
    return sorted(MODULES_DIR.rglob("text/i18n.fr.json"))


def _load_translations(path: Path) -> Dict[str, str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    source = payload.get("translations") if isinstance(payload, dict) else None
    if isinstance(source, dict):
        return {str(k): str(v) for k, v in source.items() if isinstance(k, str) and isinstance(v, str)}
    if isinstance(payload, dict):
        return {str(k): str(v) for k, v in payload.items() if isinstance(k, str) and isinstance(v, str)}
    return {}


def main() -> int:
    files = list(_iter_fr_files())
    created = 0
    entries = 0
    for fr_path in files:
        fr_map = _load_translations(fr_path)
        en_map = {k: translate(v) for k, v in fr_map.items()}
        en_path = fr_path.with_name("i18n.en.json")
        payload = {
            "locale": "en",
            "translations": en_map,
        }
        en_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        created += 1
        entries += len(en_map)
    print(f"[generate_module_i18n_en] wrote {created} files, entries={entries}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
