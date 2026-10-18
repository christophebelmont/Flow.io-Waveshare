# Sélection des binding ports au démarrage

Chaque IO slot possède une préférence matérielle : `binding_port` (primaire) et
`secondary_binding_port` (secondaire). `0` signifie « aucun candidat ». Ces champs
sont persistés ; le port sélectionné reste exclusivement en mémoire.

Le démarrage suit cet ordre :

1. Charger les valeurs par défaut du profil puis la configuration NVS.
2. Détecter les équipements nécessaires aux deux candidats. Les canaux d'un même
   équipement utilisent le même inventaire I²C / OneWire.
3. Résoudre les candidats, puis vérifier les conflits de ressources digitales.
4. Créer les endpoints et les drivers utilisés par les ports sélectionnés.

Le primaire présent et activé est prioritaire. Le secondaire est choisi si le
primaire est absent, désactivé, non configuré ou absent du catalogue compilé.
Une erreur de bus, une configuration incompatible ou un conflit ne constitue pas
une absence matérielle : ces situations restent des erreurs explicites.
Un échec d'initialisation du driver après détection ne déclenche pas de sélection
alternative. Un GPIO disponible désigne une broche de la carte, sans certifier
la présence d'un capteur câblé dessus.

**La résolution est figée pour toute la durée du boot.** Débrancher ou reconnecter
un équipement ne change jamais le port retenu. Les modifications des deux candidats
s'appliquent au prochain redémarrage. L'état courant du driver demeure distinct de
la décision prise au démarrage.

## Valeurs par défaut Waveshare

| Slot | Primaire | Secondaire | Calibration primaire C0 / C1 | Calibration secondaire C0 / C1 |
|---|---|---|---|---|
| a00 — ORP | 110, ADS externe A0–A1 | 100, ADS interne A0 | 431.03 / 0 | 129.2 / 384.1 |
| a01 — pH | 111, ADS externe A2–A3 | 101, ADS interne A1 | -2.50133333 / 6.9 | 0.9583 / 4.834 |

Les autres slots conservent leur primaire et n'ont pas de secondaire par défaut.
Les constantes de calibration viennent de `Domain/Calibration.h` ; elles ne
remplacent pas l'étalonnage des sondes. Le nom, la précision et l'identité métier
restent attachés au slot.

Les coefficients primaires conservent leurs noms existants (`aNN_c0`, `aNN_c1`).
Les coefficients du secondaire sont `secondary_c0` et `secondary_c1`. Les mises à
jour de calibration s'appliquent au candidat sélectionné, uniquement si son port
configuré correspond encore au port du boot. Une modification de port en attente
ne remplace donc pas la calibration de la route en cours.

Exemple de configuration :

```json
{
  "io/input/a00": {
    "binding_port": 110,
    "secondary_binding_port": 100,
    "a00_c0": 431.03,
    "a00_c1": 0,
    "secondary_c0": 129.2,
    "secondary_c1": 384.1
  }
}
```

Les slots digitaux utilisent le même résolveur. La polarité et le mode restent
communs aux deux candidats : ceux-ci doivent satisfaire le même contrat électrique.
Un compteur ou une entrée avec pull-down ne peut pas utiliser un port MCP23017.
Deux slots digitaux ne peuvent pas utiliser la même ressource sélectionnée ; toutes
les parties du conflit sont rejetées avant l'activation. Les sources analogiques
peuvent partager un driver et un canal de lecture.

## Migration et diagnostic

Le schéma de binding est identifié en NVS par `io_bind_v` (version 1). Les anciennes
clés de primaire et de calibration sont conservées. Pour un slot analogique ayant
déjà une affectation ou une calibration enregistrée, la migration conserve le
primaire et ses coefficients, sans ajouter de secondaire. Les anciens defaults
pH/ORP sont décrits explicitement dans le profil pour les configurations partielles.
Les slots sans configuration persistée reçoivent les nouveaux defaults.

Les valeurs migrées sont persistées avant le marqueur de version ; une écriture
échouée est journalisée et la migration est reprise au boot suivant. Le port
résolu n'est jamais persisté et aucune écriture NVS n'est faite pour une sélection.

Les métadonnées IO et les réponses `/api/io/topology` et `/api/io/runtime` exposent
les deux candidats, `resolved_binding_port`, `binding_selection`, `binding_reason`
et `binding_restart_required`. Le driver et le canal affichés suivent le port
résolu. La page Câblage affiche les candidats et le résultat du boot.

## Vérification

```sh
python3 -m unittest scripts.tests.test_io_boot_binding scripts.tests.test_i2c_addresses
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3
```

Les tests hôtes exécutent les fonctions de production : sélection, résolution figée,
validation des capacités, conflits GPIO, calibration sélectionnée, migration et
arbitrage des adresses I²C. La validation sur carte doit couvrir les boots avec et
sans carte externe, les deux cartes présentes et une configuration NVS existante.
