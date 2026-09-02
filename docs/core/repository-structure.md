# Structure du dépôt

Cette page indique où intervenir selon le type de changement. Elle complète l'[architecture générale](architecture.md) sans détailler les contrats internes de chaque module.

## Vue d'ensemble

```text
.
├── src/                    firmware et point d'entrée
│   ├── App/                bootstrap applicatif
│   ├── Board/              description matérielle statique
│   ├── Core/               services et infrastructures partagés
│   ├── Domain/Pool/        vocabulaire et règles du domaine piscine
│   ├── Modules/            fonctionnalités autonomes
│   └── Profiles/Waveshare/ assemblage de la cible matérielle
├── data/                   sources de l'interface web et du système de fichiers
├── docs/                   documentation technique et médias
├── hardware/               BOM et sources de fabrication matérielles
├── include/                fichiers de compatibilité exposés au build
├── lib/                    bibliothèques locales
├── nextion/                sources liées à l'afficheur Nextion
├── scripts/                génération et préparation du build
├── test/                   tests automatisés
├── platformio.ini          environnement et dépendances PlatformIO
└── partition_waveshare_ota_16mb.csv
```

## Firmware

### `src/App/`

Le bootstrap sélectionne explicitement l'unique profil du dépôt, prépare le contexte partagé et lance le cycle de vie applicatif. Cette couche ne porte ni règle piscine ni détail de câblage.

### `src/Board/`

La description matérielle de référence est `src/Board/WaveshareBoard.h`. Elle regroupe les UART, bus I2C et 1-Wire, le W5500, le buzzer, le TFT, les points IO et les capacités statiques.

Un changement de broche, de bus ou de périphérique compilé appartient à cette couche. Une affectation fonctionnelle configurable ne doit pas y être ajoutée.

### `src/Core/`

Le Core fournit les mécanismes partagés : registre de services, gestion des modules, configuration persistante, état runtime, bus d'événements, commandes, logs et types transverses.

Les interfaces consommées entre modules sont regroupées dans `src/Core/Services/`. Cette séparation évite qu'un module dépende directement de l'implémentation d'un autre.

### `src/Domain/Pool/`

Le domaine définit les rôles stables de la piscine : capteurs, actionneurs, équipements, valeurs par défaut et associations métier. Il exprime ce qu'est une pompe de filtration ou une sonde ORP, sans choisir directement une broche.

### `src/Modules/`

Chaque module possède un cycle de vie commun et expose sa fonction au reste du système par des services, des événements, des commandes ou des snapshots runtime.

Les dossiers `text/` placés dans les modules contiennent les libellés, traductions et descriptions de configuration utilisés par les générateurs. Ils sont des sources versionnées, contrairement aux agrégats produits pendant la compilation.

### `src/Profiles/Waveshare/`

Le profil assemble la carte Waveshare, le domaine Pool et les modules actifs. Il crée les instances, configure les points IO, associe les rôles métier aux slots logiques et enregistre les fonctions disponibles pour cette cible.

Les affectations IO par défaut sont centralisées dans `WaveshareIoLayout.h`; leur construction est réalisée dans `WaveshareIoAssembly.cpp`.

## Interface web et fichiers embarqués

`data/webinterface/` contient les sources statiques de l'interface servie par l'ESP32. La préparation du build crée des variantes compactées et des fragments dans `data/wc/`, puis assemble une arborescence SPIFFS temporaire sous `.pio/`.

Ne pas modifier manuellement les fichiers générés dans `data/wc/`. Le chemin de génération et les conventions de chargement sont détaillés dans [Ressources web Waveshare](webinterface-assets-modular.md).

## Matériel et fichiers de fabrication

`hardware/` contient les BOM normalisées, les sources EasyEDA, les Gerber, les modèles mécaniques et un catalogue avec les empreintes SHA-256. Les pages de lecture correspondantes se trouvent dans [`docs/hardware/`](../hardware/README.md).

Les fichiers tiers dont la licence de redistribution n'est pas établie ne sont pas copiés dans le dépôt : un fichier README conserve leur provenance et marque la licence **(À confirmer)**.

`nextion/releases/` contient les sources HMI et les binaires TFT versionnés par modèle. Les fichiers non versionnés conservés à la racine de `nextion/` appartiennent à l'historique du dépôt.

## Générateurs et sorties

| Élément | Statut | Source de référence |
|---|---|---|
| `scripts/` | versionné | scripts de génération et d'export |
| fichiers `text/*.json` des modules | versionnés | textes et métadonnées par module |
| `data/webinterface/` | versionné | interface web source |
| `data/wc/` | généré | générateurs de configuration/web |
| `.pio/` | généré | PlatformIO |
| `binary/` | généré | script d'export post-build |

Lorsqu'une sortie générée est incorrecte, corriger sa source ou son générateur. Ajouter une correction locale dans la sortie créerait une divergence au build suivant.

## Tests et validation

Les tests se trouvent sous `test/`. La validation de référence du firmware complet reste :

```sh
~/.platformio/penv/bin/pio run -e Flowio-waveshare-esp32-s3
```

Cette commande couvre également les générateurs exécutés avant le build et l'export post-build. Un succès de compilation ne remplace pas les essais sur carte réelle des E/S, du réseau, de l'afficheur et des mises à jour OTA.

## Où effectuer une modification

| Besoin | Emplacement à privilégier |
|---|---|
| changer une broche ou un périphérique | `src/Board/WaveshareBoard.h` |
| modifier une affectation IO par défaut | `src/Profiles/Waveshare/WaveshareIoLayout.h` |
| ajouter un rôle métier piscine | `src/Domain/Pool/` |
| modifier un comportement fonctionnel | module concerné sous `src/Modules/` |
| ajouter un contrat entre modules | `src/Core/Services/` |
| changer un texte de configuration | dossier `text/` du module concerné |
| modifier l'interface web | `data/webinterface/` |
| modifier une étape de génération | `scripts/` |

Si un changement semble nécessiter une comparaison sur des noms de chaînes, un doublon de logique ou une exception propre à un seul appelant, il faut d'abord vérifier si la responsabilité n'appartient pas à une interface, au domaine ou au profil. Le dépôt vise à conserver des dépendances explicites et des sources de vérité uniques.
