# Analyse mémoire — logs du 25 septembre 2026

L’extrait fourni montre une pression critique sur la RAM interne, mais ne contient ni panic, ni backtrace, ni redémarrage. Le motif de démarrage affiché est POWERON. Il ne permet donc pas d’attribuer formellement un crash à un échec d’allocation.

## Mesures observées

| Étape | RAM interne libre | Plus grand bloc |
| --- | ---: | ---: |
| Avant démarrage du serveur web | 63 420 octets | 55 284 octets |
| Après démarrage du serveur web | 22 952 octets | ligne tronquée |
| Requête `/api/cfgdoc/module` | 10 172 octets | 7 156 octets |
| Régime stabilisé, mémoire sous pression | 15 596 octets | 7 668 octets |

Le démarrage web consomme 40 468 octets internes supplémentaires dans cette fenêtre. MQTT suspend explicitement ses publications sous sa réserve de 16 384 octets. La PSRAM dispose encore d’environ 7 Mo. L’extrait ne montre pas une baisse continue permettant de conclure à une fuite.

Les 13 événements perdus correspondent à une saturation de la file EventBus de 40 entrées au démarrage. Les fenêtres suivantes affichent toutes zéro nouvelle perte. C’est un problème distinct de débit de traitement, pas une preuve d’épuisement du tas.

## Régression identifiée dans le changement Value

L’ELF ESP32-S3 antérieur au correctif donne `sizeof(ValueRegistry) = 7960`. Le registre est membre du DataStore statique : ses tableaux occupaient donc de la RAM interne malgré la PSRAM disponible. De plus, la pile History de 4096 octets avait été déplacée en RAM interne.

Le passage des compteurs à `uint64_t` ne suffit pas à expliquer cette consommation : les deux placements ci-dessus représentent à eux seuls environ 12 Ko internes.

## Correctif

- Les tableaux du registre occupent un seul bloc de taille fixe, alloué au démarrage du DataStore, prioritairement en PSRAM. Le mutex et le contrôle restent internes. Aucun nouvel accès par chaîne, aucune allocation lors des lectures, écritures ou transformations ; l’accès reste direct par index.
- Un repli interne existe pour les cartes sans PSRAM. Si les deux allocations échouent, l’initialisation est signalée en erreur et les opérations refusent proprement l’accès au stockage absent.
- La pile History utilise la PSRAM lorsqu’elle existe, sinon la RAM interne. Les lectures NVS de restauration précèdent le démarrage de cette tâche ; ses écritures sont asynchrones et exécutées par la tâche ConfigStore en RAM interne.
- Le démarrage journalise la taille et l’emplacement du registre pour vérifier le placement sur la carte.

PCNT, son filtrage et son absence d’ISR ajoutée sont conservés. La file EventBus et les réserves MQTT ne sont pas agrandies ou abaissées pour masquer les symptômes.

## Validation et limites

Les tests hôtes avec AddressSanitizer et UndefinedBehaviorSanitizer couvrent le placement PSRAM, le repli interne, l’épuisement mémoire, l’absence de nouvelles allocations en fonctionnement et les accès concurrents au registre, ainsi que les compteurs et l’historique.

La compilation `Flowio-waveshare-esp32-s3` réussit : RAM statique 91 572 octets, flash 2 255 842 octets. Dans le nouvel ELF, `sizeof(ValueRegistry) = 124` et le bloc déplacé mesure 7840 octets. La réduction propre au registre est donc de 7836 octets internes, auxquels s’ajoutent les 4096 octets de pile History déplacés, soit environ 11,7 Kio attendus sur cette carte.

Le gain de RAM interne attendu doit être confirmé sur carte après démarrage du web et sous charge HTTP/MQTT. Une trace complète du panic, accompagnée de l’ELF correspondant, reste nécessaire pour identifier le crash signalé. Ce correctif ne constitue pas une preuve de résolution de tous les crashes.

## Notifications initiales EventBus

Les changements DataStore produits avant le lancement des tâches ne remplissent plus la file EventBus. `StartupDataChanges` conserve un bit par `DataKey`, avec une section critique courte pour les producteurs concurrents. La tâche EventBus consomme les événements ordinaires puis tente quatre publications initiales par passage. Une tentative refusée conserve la clé en attente. Le parcours circulaire évite qu’une clé fréquemment modifiée monopolise la publication.

Après vidage des clés initiales, les notifications DataStore retrouvent leur chemin habituel. Les commandes, alarmes et transitions ne sont pas regroupées. `SystemStarted` est conservé séparément jusqu’à acceptation par la file. `tryPost` ne compte pas une file pleine comme une perte lorsque son appelant conserve explicitement l’événement pour réessayer ; `post` conserve le comptage des pertes réelles.

Validation hôte : saturation préalable des 40 entrées de la véritable implémentation EventBus (API FreeRTOS simulée), publication de toutes les clés, doublons initiaux regroupés, mises à jour concurrentes pendant acceptation/refus, ordre des événements ordinaires, un seul `SystemStarted`, comptage des débordements ordinaires inchangé. L’absence de pertes sur le démarrage matériel complet reste à confirmer dans les logs de la carte.
