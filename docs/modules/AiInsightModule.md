# AiInsightModule

`AiInsightModule` prépare et produit sur demande une explication de l’état du
bassin. Il porte la configuration OpenAI, récupère un résumé météo borné,
construit le prompt à partir de la météo et de `PoolHistoryService`, puis appelle
l’API OpenAI Responses sans bloquer le serveur web.

## Configuration

Le module enregistre cinq valeurs persistantes dans `ConfigStore` :

- `ai/openai/enabled` : activation de la fonctionnalité ;
- `ai/openai/api_key` : clé API conservée comme une chaîne ordinaire ;
- `ai/openai/model` : identifiant du modèle à employer ;
- `system/location/latitude` : latitude en degrés décimaux ;
- `system/location/longitude` : longitude en degrés décimaux.

La latitude et la longitude sont stockées en `double`, exportées avec six
décimales et présentées dans l’interface avec un pas de `0,000001°`. La valeur
initiale de latitude est volontairement hors plage : aucune requête météo ne
peut partir tant qu’un emplacement valide n’a pas été configuré.

Conformément au choix fait pour ce prototype privé, le module n’ajoute pas de
mécanisme de chiffrement ou de coffre de secrets.

## Contexte météo

`OpenMeteoWeatherClient` interroge l’endpoint HTTPS de prévision à partir des
coordonnées configurées. La réponse est réduite à :

- température, couverture nuageuse et vent actuels ;
- températures minimale et maximale des 24 heures précédentes et suivantes ;
- cumul des précipitations passées et prévues ;
- couverture nuageuse moyenne, vent maximal et rayonnement solaire moyen prévus.

Le parseur refuse les tableaux horaires absents, désalignés ou supérieurs à 64
échantillons. Le corps HTTP est limité à 14 Kio et le document JSON à 16 Kio.
Ces deux stockages, ainsi que l’état météo, résident en PSRAM sans repli vers la
RAM interne.

Une donnée réussie est conservée pendant trente minutes pour le même
emplacement. Une indisponibilité réseau ou météo place la demande en échec sans
bloquer le module ni effacer le dernier instantané valide.

Chaque relève journalise l’interface réseau par défaut, son adresse locale, sa
passerelle et son serveur DNS, puis le résultat de la résolution de
`api.open-meteo.com`. En cas d’échec HTTPS, le diagnostic contient le libellé
`HTTPClient`, le code de transport et le détail fourni par mbedTLS. Les réponses
HTTP réussies journalisent leur taille annoncée et la validation du JSON.

## Service et diagnostic

Le contrat `AiInsightService`, enregistré sous `ServiceId::AiInsight`, permet de
demander un rafraîchissement asynchrone et de lire son état. Les commandes
`ai.weather.refresh` et `ai.weather.status` offrent le même diagnostic pendant
la mise au point. Une demande forcée ignore le cache ; le futur flux d’analyse
pourra utiliser le rafraîchissement non forcé.

## Aperçu du prompt

`PoolInsightPromptBuilder` produit deux chaînes bornées : un résumé météo
lisible et le prompt complet. Le prompt place les instructions stables avant
les données dynamiques et demande une explication courte de l’état et de la
dynamique du bassin. Il interdit explicitement les consignes de modification
du pH, de l’ORP ou des automatismes déjà pilotés par Flow.io.

La route locale `GET /api/ai/pool-preview` expose cet aperçu, l’état de
l’analyse et son dernier texte. Le paramètre `refresh=1` planifie auparavant un
rafraîchissement météo non forcé.

Les grandes chaînes de l’aperçu sont matérialisées dans une structure allouée
en PSRAM pour la durée de la requête. La réponse JSON asynchrone utilise elle
aussi un buffer de 24 Kio en PSRAM ; aucune copie volumineuse n’est placée sur
la pile ou en RAM interne.

## Génération OpenAI

`POST /api/ai/pool-insight` met une analyse en file d’attente et répond
immédiatement avec le statut HTTP `202`. La tâche `ai.insight` actualise la météo
si nécessaire, reconstruit ensuite le prompt avec l’historique le plus récent,
puis appelle `POST https://api.openai.com/v1/responses` avec le modèle configuré.

La requête utilise un `input` texte simple, limite la génération à 500 jetons et
positionne `store` à `false`. Le parseur parcourt la collection `output` du
schéma Responses et concatène uniquement les blocs `output_text` des messages.
Les erreurs HTTP OpenAI sont extraites depuis `error.message`, avec `error.code`
ou `error.type` comme discriminant. Le diagnostic reste exploitable même si le
message doit être tronqué à la capacité bornée de l’état. Les refus HTTP
journalisent également `x-request-id` et, lorsqu’ils sont fournis, les compteurs
et délais de réinitialisation de limite de débit renvoyés par OpenAI.

L’état suit la séquence `idle -> queued -> loading -> ready|failed`. La page
Piscine interroge cet état pendant au plus 90 secondes et affiche le résultat
sous forme de texte simple. Aucun code ou corpus de réponses local n’est
appliqué au texte retourné.

Une analyse réussie est réutilisée pendant les 60 minutes suivant sa génération.
Pendant cette période, une nouvelle action utilisateur renvoie immédiatement le
résultat existant et ne déclenche ni nouvelle requête météo ni appel OpenAI. À
60 minutes révolues, une nouvelle analyse peut être mise en file d’attente.

Le prompt inclut les sept journées closes, leur couverture, les statistiques
jour/nuit, la filtration, le remplissage, les caractéristiques du bassin et la
méthode de désinfection active. La journée en cours reste fournie uniquement
comme contexte complémentaire.

Les buffers temporaires de requête HTTP (24 Kio), réponse HTTP (24 Kio),
documents JSON (16 et 32 Kio), prompt (12 Kio) et résultat (4 Kio) utilisent tous la
PSRAM. Le texte final est conservé dans le stockage PSRAM du module jusqu’à la
prochaine analyse ou au redémarrage. Il n’est pas persisté en flash : la fenêtre
de réutilisation d’une heure ne traverse donc pas un redémarrage de l’ESP32.
