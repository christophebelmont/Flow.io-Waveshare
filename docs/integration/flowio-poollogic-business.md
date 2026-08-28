# flow.io — logique métier piscine du profil Waveshare

Ce document résume la logique métier compilée dans l'environnement `Flowio-waveshare-esp32-s3`, avec la carte `WaveshareESP32S3` et le domaine `Pool`.
Il complète la documentation module par module avec une vue orientée fonctionnement du bassin : filtration, hivernage, injection pH/ORP, électrolyse, chauffage, robot et remplissage.

## 1) Contexte de compilation

Le profil Waveshare assemble:

- Carte : `WaveshareESP32S3`
- Domaine: `Pool`
- Modules critiques métier: `IOModule`, `PoolLogicModule`, `PoolDeviceModule`, `TimeModule`, `AlarmModule`, `CommandModule`, `MQTTModule`, `HAModule`.

Le flux de décision principal est:

1. `PoolLogicModule` calcule les **intentions métier** (désirs ON/OFF des équipements + régulation PID).
2. `PoolDeviceModule` applique ces demandes avec les **interlocks** (dépendances, max uptime, activation appareil, I/O error), puis pilote les sorties via `IOModule`.
3. `IOModule` traduit les points logiques en ressources physiques sélectionnées par le profil Waveshare.

## 2) Affectations E/S par défaut du profil Waveshare

### 2.1 Sorties physiques (DO)

Les huit premières sorties logiques sont reliées par défaut au TCA9554 de la carte Waveshare :

| IO slot | Binding port | Fonction piscine |
|---|---|---|
| `d00` | `EXIO1` | pompe de filtration |
| `d01` | `EXIO2` | pompe pH |
| `d02` | `EXIO3` | pompe chlore/brome ou oxygène actif |
| `d03` | `EXIO4` | robot |
| `d04` | `EXIO5` | remplissage |
| `d05` | `EXIO6` | électrolyseur |
| `d06` | `EXIO7` | éclairage |
| `d07` | `EXIO8` | chauffage |

Les slots `d08..d15` correspondent aux sorties du MCP23017 et n'ont pas de rôle métier Pool prédéfini.

### 2.2 Entrées capteurs (AI/DI)

Affectation logique Pool par défaut :

- analogiques : `a00` ORP, `a01` pH, `a02` pression, `a03` libre, `a04` température eau, `a05` température air, `a06` courant et `a07` tension ;
- digitales : `i08` PIR, `i09` niveau cuve pH, `i10` niveau cuve désinfection, `i11` niveau bassin et `i12` compteur d'eau.

Les slots numériques restent stables même si leur `binding_port` est modifié en configuration. La [cartographie IO](../core/waveshare-io-map.md) donne les ports physiques, les ressources libres et les réservations du TFT.

Convention logique harmonisée pour les entrées de niveau :
- `ON` (`true`) = alerte/problème (niveau bas)
- `OFF` (`false`) = pas de problème

## 3) Rôles des 3 modules clés

## 3.1 `PoolLogicModule` (orchestrateur métier)

Responsabilités:

- calcule la fenêtre de filtration (selon température eau + bornes horaires)
- arbitre mode auto/manuel/hiver
- applique sécurités pression PSI
- pilote robot, électrolyse, remplissage
- exécute la régulation temporelle PID pH et ORP (pompes péristaltiques)
- exécute le protocole oxygène actif liquide par volume calculé quand `disinfection_type=2`
- publie la configuration `cfg/poollogic/*` et snapshots `rt/poollogic/ph|orp`

Important: en mode manuel (`auto_mode=false`), la filtration reste pilotée manuellement **sauf sécurité PSI** (qui garde priorité et peut couper).

## 3.2 `PoolDeviceModule` (device manager / couche d’exécution)

Responsabilités:

- expose le service `PoolDeviceService` utilisé par `PoolLogic`
- applique les commandes ON/OFF demandées sur les slots `pd0..pd7`, chacun associé à un rôle Pool prédéfini
- impose des contraintes d’exécution:
  - `enabled` par appareil
  - dépendances (`depends_on_mask`)
  - `max_uptime_day_s`
  - état I/O réel
- maintient compteurs runtime:
  - temps de marche jour/semaine/mois/total
  - volume injecté jour/semaine/mois/total
  - volume restant cuve
- persiste les métriques via les blobs runtime NVS du ConfigStore

## 3.3 `IOModule`

Responsabilités:

- lit les analogiques/digitales utilisées par la logique pool
- écrit les sorties digitales physiques
- maintient la config des bindings (`binding_port`) + calibration analogique (`c0`,`c1`,`precision`) + logique d’entrée digitale (actif haut, pull, edge/counter)

## 4) Modes métier (auto, hiver, manuel)

## 4.1 Auto global (`auto_mode=true`)

- filtration demandée par:
  - fenêtre scheduler calculée
  - ou mode hiver + condition de température air
  - avec logique de maintien hors-gel (`freeze_hold_t`) si la filtration tourne déjà
- robot, électrolyse, PID pH/ORP sont pilotés automatiquement

## 4.2 Manuel (`auto_mode=false`)

- `PoolLogic` n’impose plus de logique auto de filtration
- un ordre manuel filtration (`poollogic.filtration.write`) force explicitement `auto_mode=false`
- les sécurités PSI restent actives (coupure possible)

## 4.3 Hiver (`winter_mode=true`)

- ajout d’une demande de filtration quand `air_temp < winter_start_t`
- logique hors-gel: si filtration déjà active et `air_temp <= freeze_hold_t`, elle reste maintenue

## 5) Détail des équipements

## 5.1 Filtration (pompe principale)

Décision finale (priorité):

1. sécurité PSI (stop)
2. manuel (conserver état manuel)
3. auto (fenêtre de filtration / hiver / freeze-hold)

La fenêtre est recalculée quotidiennement et peut être relancée via commande.

## 5.2 Injection pH (pompe pH)

Conditions d’autorisation:

- filtration demandée ON
- PID pH armé (`ph_auto_mode` + délai post-filtration)
- mesure pH disponible
- pas d’erreur PSI
- cuve pH non vide

Le PID est temporel (sortie en `outputOnMs` dans une fenêtre), avec minimum ON configurable (`pid_min_on_ms`).

`ph_dose_plus` inverse le sens d’erreur (injection pH+ vs pH-).

## 5.3 Injection ORP / chlore liquide (pompe ORP)

Conditions d’autorisation:

- filtration demandée ON
- PID désinfection armé (`dis_auto_mode` + délai post-filtration)
- mesure ORP disponible
- pas d’erreur PSI
- cuve chlore non vide
- **et** `disinfection_type=0` (`Chlore/Brome`; la pompe ORP automatique est inhibée en mode électrolyse, oxygène actif ou désactivé)

`disinfection_type=3` (`Désactivé`) coupe la désinfection automatique. Un démarrage manuel de la pompe chlore/O2 bascule aussi ce champ sur `Désactivé`, afin de laisser la main à l'utilisateur sans relance automatique de dosage.

## 5.4 Électrolyseur

En auto:

- nécessite filtration ON
- nécessite `disinfection_type=1` (`Electrolyse`)
- démarrage autorisé si:
  - température eau >= `secure_elec_t`
  - délai filtration >= `dly_electro_min`
- si `swg_control_mode=0`, l’électrolyse est asservie à ORP (hystérésis de démarrage à 90% de la consigne)
- si `swg_control_mode=1`, l’électrolyse fonctionne en continu sur la plage autorisée

## 5.5 Oxygène actif liquide

En auto:

- nécessite `disinfection_type=2` (`Oxygène actif`)
- n'utilise pas la sonde ORP
- calcule une dose hebdomadaire à partir de:
  - `pool_volume_m3`
  - `dose_ml_10m3_week`
  - `load_factor`
  - compensation température optionnelle (`temp_comp`)
- fractionne la dose selon `split_count`:
  - `1`: lundi
  - `2`: lundi et jeudi
  - `3`: lundi, mercredi et vendredi
- crée une dose en attente à partir de `main_hour`
- demande la filtration si une dose est en attente
- attend `min_filter_run_min` minutes de filtration effective
- pilote la pompe de désinfection au volume, avec le débit `flow_l_h` configuré dans `PoolDeviceModule`

Le protocole persiste `protocol_state`, `last_dose_day`, `weekly_done_ml` et `pending_ml` pour reprendre après reboot. La limite `max_uptime_day_s` de la pompe reste appliquée par `PoolDeviceModule`; si elle est atteinte, l'O2 est bloqué avec la raison `pump_blocked`.

## 5.6 Robot

En auto:

- ne démarre que si filtration ON et nettoyage non fait du jour
- démarre après `robot_delay_min`
- s’arrête après `robot_dur_min`
- `cleaning_done` est remis à false au changement de jour

## 5.7 Remplissage

- basé sur le capteur de niveau bassin (`i11` par défaut)
- si le niveau bas est actif : marche
- quand niveau revient OK: maintien ON jusqu’à atteindre au moins `fill_min_on_s`

## 6) Sécurité / alarmes

Alarmes métier déclarées:

- `PoolPsiLow` (pression basse)
- `PoolPsiHigh` (pression haute)
- `PoolWaterLevelLow` (niveau bassin bas)
- `PoolPhTankLow`
- `PoolChlorineTankLow`

Si le service alarme n’est pas disponible, `PoolLogic` utilise un fallback local conservatif (latch PSI local).

## 7) Variables ConfigStore importantes

## 7.1 `poollogic/*`

### `poollogic/modes`

- `enabled`
- `auto_mode`
- `winter_mode`
- `disinfection_type`

### `poollogic/ph`

- `ph_auto_mode`
- `ph_dose_plus`
- `ph_setpoint`
- `ph_kp`, `ph_ki`, `ph_kd`
- `ph_window_ms`

### `poollogic/chlorine`

- `dis_auto_mode`
- `dis_setpoint`
- `dis_kp`, `dis_ki`, `dis_kd`
- `dis_window_ms`

### `poollogic/swg`

- `swg_control_mode`
- `secure_elec_t`
- `dly_electro_min`

### `poollogic/o2`

- `pool_volume_m3`
- `dose_ml_10m3_week`
- `load_factor`
- `temp_comp`
- `split_count`
- `main_hour`
- `min_filter_run_min`
- `protocol_state`
- `last_dose_day`
- `weekly_done_ml`
- `pending_ml`

### `poollogic/filtration`

- `wat_temp_lo_th`
- `wat_temp_setpt`
- `filtr_start_min`
- `filtr_stop_max`
- `filtr_start_clc`
- `filtr_stop_clc`

### `poollogic/sensors`

- `ph_io_id`
- `dis_io_id`
- `psi_io_id`
- `wat_temp_io_id`
- `air_temp_io_id`
- `pool_lvl_io_id`
- `ph_lvl_io_id`
- `chl_lvl_io_id`

### `poollogic/safety`

- `psi_low_th`
- `psi_high_th`
- `winter_start_t`
- `freeze_hold_t`
- `psi_start_dly_s`

### `poollogic/regulation`

- `dly_pid_min`
- `pid_min_on_ms`
- `pid_sample_ms`

### `poollogic/heater`

- `heater_auto_mode`
- `heater_setpoint`

### `poollogic/robot`

- `robot_delay_min`
- `robot_dur_min`

### `poollogic/refill`

- `fill_min_on_s`

### `poollogic/devices`

- `filtr_slot`
- `swg_slot`
- `robot_slot`
- `fill_slot`
- `ph_pump_slot`
- `dis_pump_slot`

## 7.2 `pdm/*` (device manager)

Par slot `pdm/pdN` (N=0..7):

- `enabled`
- `depends_on_mask`
- `flow_l_h`
- `tank_cap_ml`
- `tank_init_ml`
- `max_uptime_day_s`

Runtime persistant par slot, stocké en blob NVS interne `pdNrt`:

- blob de métriques

## 7.3 `io/*` (principalement utile à la logique piscine)

- activation globale: `io/enabled`
- capteurs analogiques `io/input/a0..a5`:
  - `*_name`, `binding_port`, `*_c0`, `*_c1`, `*_prec`
- entrées digitales `io/input/i0..i4`:
  - `*_name`, `binding_port`, `*_active_high`, `*_pull_mode`, `edge_mode`
- sorties `io/output/d0..d7`:
  - `*_name`, `binding_port`, `*_active_high`, `*_initial_on`, `*_momentary`, `*_pulse_ms`

## 8) Commandes métier principales

- `poollogic.filtration.write`:
  - force `auto_mode=false`
  - écrit directement la demande filtration
- `poollogic.filtration.recalc`:
  - demande recalcul de la fenêtre de filtration
- `poollogic.auto_mode.set`:
  - bascule explicite mode auto
- `pooldevice.write`:
  - commande directe d’un slot appareil
- `pool.refill`:
  - réinitialise niveau cuve (pompes doseuses)

## 9) Résumé opérationnel

- `PoolLogic` décide **quoi faire** (métier bassin).
- `PoolDevice` décide **si c’est autorisé** et applique **comment** côté actionneurs.
- `IO` traduit les slots logiques en lectures de capteurs et écritures de sorties du profil Waveshare.

Cette séparation permet:

- une logique métier claire et configurable (`poollogic/*`)
- une sécurité d’exécution centralisée (`pdm/*`)
- une adaptation matérielle centralisée (`io/*` et bindings Waveshare).
