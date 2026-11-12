# Forçage temporaire des équipements ON/OFF

Tous les PoolDevices équipés d'un pilote relais sont éligibles, indépendamment de
leur rôle, nom, langue ou présence sur le tableau de bord. Un relais utilisé
par plusieurs équipements est exclu pour éviter plusieurs propriétaires.
Les sorties digitales associées projettent le même état. Les sorties libres,
les consignes variables, les pilotes multi-vitesses et RS485 ne sont pas concernés.

## Commande et interface web

« Gérer les équipements » propose **Forcer…** sur chaque équipement compatible,
même sans mode automatique. Le panneau compact permet Marche/Arrêt et une durée
de 1 à 1 440 minutes. **Modifier…** remplace la demande ; **Terminer le forçage**
revient à la consigne guidée. Un badge apparaît seulement lors d'un forçage,
d'une attente d'heure ou d'une erreur. L'état réel reste présenté séparément.

Le tableau de bord garde ses commandes manuelles sans durée, avec leur gestion
historique du mode automatique. Une commande manuelle validée remplace la consigne
et annule le forçage dans une même transaction, sans appliquer brièvement l'ancienne
consigne guidée entre les deux. Cliquer sur la carte ouvre la gestion ; seul le
toggle envoie une commande. Chaque demande temporaire ON/OFF acceptée est journalisée
avec sa durée, son équipement et son auteur web ou Remote (MQTT).

## Arbitrage et protections

PoolDevice possède un temporisateur par équipement. Les automatismes continuent
à mettre à jour la consigne guidée pendant le forçage ; le forçage ne l'écrase pas.
À l'échéance, la consigne guidée courante reprend, y compris si elle demande ON.
Un équipement sans automatisme retrouve sa consigne manuelle précédente.

PoolDevice applique ensuite les protections du pilote, les dépendances et le
maximum journalier. PoolLogic fournit des contraintes par slot : pression,
maintien antigel pour un OFF filtration, filtration réellement en marche pour
les appareils dépendants, température valide, réservoirs de dosage et niveau.
Les contraintes de rôles partageant un slot se cumulent. Une protection annule
le forçage concerné ; elle ne met pas le délai en pause et ne le réarme pas.
Le forçage ne démarre jamais implicitement une dépendance.

Au démarrage, un forçage restauré attend la récupération de ses dépendances.
Après sa première application, leur perte annule le forçage. Cela évite que
l'ordre numérique des slots décide de la reprise de forçages simultanés.

## Persistance

Chaque demande est sauvegardée avant son acceptation, avec son slot, la liaison
physique, la consigne, le début et l'échéance UTC. Le format explicite du timer
(32 octets, versionné) est suivi d'un octet de consigne manuelle de référence.
Clés NVS : `pdm_ovr_N`, préférence de durée `pdm_ovr_min`.
Le décompte seul ne provoque aucune écriture flash.

Une heure RTC fiable ou NTP est nécessaire. Sans elle au reboot, l'état est
`waiting_time` et le relais reste arrêté. Le temps hors tension compte dans le
délai. L'expiration et l'annulation sauvegardent une marque inactive ; une erreur
d'écriture arrête l'application, expose `persistence_error` et déclenche une
nouvelle tentative périodique. Un redémarrage avant que cette marque ait pu être
écrite reste soumis à l'ancien enregistrement et à son échéance : une flash en
échec ne peut pas garantir la durabilité d'une annulation non confirmée.

Les anciens enregistrements `ovr_filtr` et `ovr_robot` sont migrés selon leur slot
enregistré. Le nouvel enregistrement est écrit avant adoption ; sa présence,
même inactive, interdit toute nouvelle migration de l'ancien enregistrement.
Un changement de liaison physique annule la demande restaurée.

## MQTT

Publier sans rétention sur `<base>/<device>/cmd` :

```json
{"cmd":"pooldevice.override","args":{"slot":9,"value":true,"duration_s":1800}}
{"cmd":"pooldevice.override_off","args":{"slot":9,"duration_s":900}}
{"cmd":"pooldevice.release","args":{"slot":9}}
```

Une cible explicite exige une durée explicite (1 à 86 400 secondes), sauf pour
la libération. L'ACK confirme l'acceptation durable, pas la marche physique.
Les anciennes commandes `poollogic.device.override*` et `.release` restent des
adaptateurs vers le même propriétaire. Les rôles `filtration` et `robot` sont
résolus à partir de la configuration actuelle, pas du nom affiché.

Le panneau Discovery utilise `pooldevice.override.select` avec l'option exacte
annoncée dans le catalogue, `pooldevice.override.duration.set` avec `minutes`,
puis `pooldevice.override_on`, `.override_off` ou `pooldevice.release` sans cible.
La sélection n'agit jamais sur un relais. Elle est globale et volatile ; pour
les automatisations, préférer impérativement une cible explicite.

Snapshots communs : `rt/pdm/override/selected` et `rt/pdm/override/all`.
Le premier expose le slot sélectionné, sa consigne et ses attributs. Le second
expose `active_count` et un objet `actuators` indexé par `pdN`, contenant tous les
équipements compatibles. Les changements de commande sont publiés immédiatement.
Le décompte est rafraîchi toutes les dix secondes pendant un forçage actif.
Les topics historiques `rt/poollogic/override/filtration` et
`rt/poollogic/override/robot` suivent chacun leur équipement et la durée par défaut.

Les états individuels `rt/pdm/state/pdN` et la sortie digitale associée exposent
`override_supported`, `override_available`, `control_automatic`, `control_mode`,
`override_reason`, `override_value`, `override_remaining_s`, `override_ends_at_utc`.
Un changement du temps restant seul déclenche également une publication toutes
les dix secondes sur ces topics. Les transitions de commande sont immédiates.
`control_mode` vaut `guided`, `forced`, `waiting_time` ou `persistence_error`.
Ces codes et les booléens ne sont jamais traduits ; les interfaces traduisent
leur présentation uniquement.

Le topic `rt/pdm/state/pdN` expose aussi le booléen `forced`, repris dans les
attributs du switch Home Assistant. Il vaut `true` pendant un forçage en marche
ou à l'arrêt, puis `false` dès la fin du forçage.

Voir [la carte Home Assistant commune](../examples/home-assistant/README.md).
