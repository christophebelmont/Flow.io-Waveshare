# Mode numérique 2 — Débit instantané (Flow.io Waveshare ESP32-S3)

**Branche de travail :** `experiment/modbus-tcp-psram-buffers-20261122`  
**Statut :** implémenté pour tests ; compilation complète ESP32-S3 et validation sur les équipements réels requises.

## Expérience de configuration

`Mode entrée` propose :
- `0 | État` : fonctionnement binaire historique, non modifié ;
- `1 | Compteur d'impulsions` : cumul NVS, remise à zéro et modification manuelle, non modifié ;
- `2 | Débit instantané` : mesure de fréquence et débit calculé, **sans compteur cumulatif exposé ni checkpoint NVS**.

En mode 2, l'utilisateur conserve la configuration GPIO, `Actif à 1`, le pull, la sélection de front, le filtre de rebonds, `Coef C0` et `Précision`. Le menu `Unité` propose `l/min`, `l/h`, `m3/min`, `m3/h`. Les commandes `Remettre le compteur à zéro` et `Valeur du Compteur` ne s'affichent pas : elles n'ont aucune fonction dans ce mode.

Le changement de mode ou d'unité s'applique au prochain redémarrage, comme les autres changements d'affectation du driver.

## Calcul physique et convention K

**K / C0 = litres par impulsion**, valeur non négative et finie. Cette convention évite d'avoir un K ambigu quand on change l'unité choisie.

```
                  GPIO d'entrée
                        │
             PCNT matériel ESP32-S3
                        │ lecture toutes les >= 20 ms
           Δ impulsions sur ~1 seconde
                        │
             impulsions / minute
                        │
                  × C0 (L/pulse)
                        │
                litres / minute
                        │ sélection d'unité
        ┌───────────────┼──────────────────┐
      l/min             l/h             m3/min          m3/h
      × 1               × 60             ÷ 1000          × 0,06
```

Exemple : 10 impulsions en 1 seconde et C0 = 0,5 litre/impulsion ⇒ fréquence 600 impulsions/min ⇒ 300 l/min, 18 000 l/h, 0,300 m3/min ou 18 m3/h.

L'échantillonnage tient compte de la durée réelle de la fenêtre, pas d'un délai nominal fixe : `rate = ΔP × 60 000 / Δt_ms`. Une fréquence nulle pendant une seconde donne un débit nul. Les changements de valeur sont quantifiés à la précision configurée pour les affichages.

Le compteur matériel a besoin d'une base temporaire pour calculer `ΔP` : elle existe **uniquement en RAM**, n'est ni exposée comme cumul au système, ni sauvegardée en NVS. Une éventuelle ancienne valeur de checkpoint issue d'un précédent mode 1 n'est pas incrémentée en mode 2.

## Chemin de publication

```
PCNT → tâche IO → PulseRateOnly → PulseFlowUnits → ValueRegistry
                                                 │
                            ValueIds::PulseRate+i (iXX.rate)
                            ValueIds::ConvertedRate+i (iXX.flow)
                                                 │
                        ┌────────────────────────┼─────────────────────┐
                   Vue Web principale          TFT             MQTT / HA Discovery
```

`iXX.flow` conserve son identifiant stable ; par exemple `i01.flow` = ValueId 81, topic suffixe `rt/value/81`. Il n'y a **pas de nouveau widget TFT** : on choisit cette valeur dans la liste de données déjà disponible. `ValueMetadata.displayUnit` transmet l'unité configurée aux vues et à la découverte Home Assistant. Les publications HA de `iXX.flow` et de la fréquence brute `iXX.rate` (imp/min) sont proposées par défaut, tout en respectant une désactivation antérieure sauvegardée dans les paramètres d'exposition. Les deux grandeurs ont des ValueIds stables.

`iXX.count` et `iXX.total` ne sont pas créés pour un équipement en mode 2 ; les modes existants conservent leurs identifiants et comportements.

## Budget CPU/mémoire

- **Hypothèse matériel de production : au maximum deux entrées en mode 2.**
- Le PCNT matériel compte les fronts indépendamment des tâches logicielles.
- Le driver est interrogé à la cadence numérique déjà utilisée (minimum 20 ms), soit au maximum ~100 lectures/s pour deux entrées.
- Deux calculs fréquence + conversion d'unité par seconde, hors première lecture ou rebase ; pas de nouveau thread/FreeRTOS, pas de heap ni allocations de chaînes par échantillon.
- Les valeurs sont écrites en ValueRegistry à l'expiration de la fenêtre de mesure, non 50 fois/s. Aucun checkpoint ni écriture Flash périodique en mode 2.
- Le budget CPU réel dépend aussi du TFT, du réseau, du séquenceur RS485/Modbus, des logs et des OTA. **Il devra être vérifié sur la carte** par comparaison des marges de tâche, temps de cycle I/O, charge CPU et latences dans des scénarios à 0 et 2 entrées en mode 2.

## Tests

Le test `scripts/tests/test_pulse_flow_rate.py` compile `test/host/pulse_rate_only/scenarios.cpp` avec les vrais `PulseRateOnly.h` et `PulseFlowUnits.h`, vérifie :
- deux entrées en parallèle, cadence d'acquisition 20 ms, sortie rafraîchie toutes ~1 seconde ;
- les quatre conversions, la valeur C0 = 0, les entrées/unités invalides ;
- arrêt d'impulsions (retour à 0), rebase du PCNT, dépassement du compteur de millisecondes ;
- câblage statique du chemin IO/ValueRegistry, sélection Web et identité de donnée.

Le test algorithmique isolé a été compilé et exécuté avec succès sur hôte Linux, avec `-Wall -Wextra -Werror -fsanitize=address,undefined` ; **cela ne valide pas une compilation PlatformIO ESP32-S3 complète**.

À vérifier ensuite sur matériel : mode 0 et 1 inchangés, changement de mode après reboot, rebonds, 2 PCNT actifs, les quatre unités dans les quatre destinations, publication HA avec NVS déjà configuré, charge réseau + TFT + Modbus + OTA, aucune écriture de compteur dans le mode 2.
