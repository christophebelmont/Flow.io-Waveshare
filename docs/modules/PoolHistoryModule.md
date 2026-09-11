# PoolHistoryModule

`PoolHistoryModule` agrège l’historique journalier destiné aux consommateurs
d’analyse, notamment `AiInsightModule`. Il travaille sur le jour local courant
et les sept dernières journées calendaires closes.

## Échantillonnage

Le pH, l’ORP et la température d’eau sont échantillonnés toutes les cinq
minutes uniquement lorsque la filtration est réellement en marche depuis au
moins dix minutes sans interruption. Un arrêt, un état de pompe inconnu ou un
trou d’observation supérieur à cinq minutes remet ce délai à zéro. La
température d’air reste indépendante de cette règle.

Chaque journée conserve, pour les trois mesures d’eau, le nombre
d’échantillons, la première et la dernière valeur, le minimum, le maximum et la
moyenne. La température d’eau possède aussi deux agrégats séparés : journée et
nuit. La journée est configurable par `poolhistory/periods/day_start_hour` et
`day_end_hour` (08:00–20:00 par défaut). La variation publiée est signée :
`moyenne nuit - moyenne jour`.

Les consignes pH, ORP et chauffage sont échantillonnées dans le même historique
typé. Chaque journée en conserve le début, la fin, le minimum, le maximum et la
moyenne. La configuration actuelle et les modes automatiques restent exposés
une seule fois dans l’instantané global.

## Filtration et remplissage

Le module cumule en un seul passage les durées réelles de filtration, chauffage
et remplissage. La filtration et le chauffage conservent séparément leur durée
active et leur durée réellement observée pour quatre périodes locales fixes :
nuit 00:00–06:00, matin 06:00–12:00, après-midi 12:00–18:00 et soir
18:00–24:00. Les totaux restent exposés en secondes, minutes et heures.
Un événement de remplissage correspond à une transition constatée de la pompe
de remplissage de l’arrêt vers la marche.

Le volume de remplissage est calculé avec le débit `flow_l_h` déjà configuré
sur le slot PoolDevice de remplissage (`pdm/pd4`). Si la pompe fonctionne avec
un débit nul ou invalide, le nombre d’événements reste disponible mais le
volume de la journée et le total sur sept jours sont marqués indisponibles.

La synthèse sur sept jours contient le temps total de filtration, sa moyenne
sur les journées réellement disponibles, le volume total et moyen de
remplissage, ainsi que le nombre total d’événements. Le nombre de journées
disponibles est toujours exposé pour ne pas confondre absence de données et
valeur nulle.

## Caractéristiques du bassin

`PoolLogicModule` publie `PoolConfigurationService`. L’instantané historique
reprend le volume déjà configuré pour le bassin, le caractère intérieur ou
extérieur, la présence d’une couverture automatique, sa fermeture nocturne et
la méthode de désinfection active. Les trois caractéristiques booléennes sont
configurables sous `poollogic/pool`; la méthode active provient directement de
`poollogic/modes/disinfection_type`.

## Persistance et mémoire

Un anneau fixe contient aujourd’hui et sept jours clos, sans allocation pendant
les boucles d’agrégation. Chaque journée est persistée séparément dans
`ConfigStore` afin de rester compatible avec les écritures asynchrones bornées.
Le format binaire version 3 occupe 368 octets et conserve signature, version et
checksum. Les versions antérieures ne sont volontairement pas reconnues : la
NVS doit être effacée lors de cette mise à niveau.

L’accumulateur, les huit journées chargées, les zones de copie de persistance
et les buffers de sérialisation sont contenus dans `PoolHistoryModule::Storage`,
alloué exclusivement avec `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. Il n’existe
aucun repli vers la RAM interne. Les instantanés volumineux utilisés par l’IA
sont eux aussi hébergés dans les structures de travail PSRAM du module IA.
