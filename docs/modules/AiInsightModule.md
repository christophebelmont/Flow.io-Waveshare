# AiInsightModule

`AiInsightModule` prépare le contexte nécessaire à la future explication de
l’état du bassin. À ce stade, il porte la configuration OpenAI et récupère un
résumé météo borné. Il n’appelle pas encore l’API OpenAI et ne produit aucun
texte de préconisation.

## Configuration

Le module enregistre cinq valeurs persistantes dans `ConfigStore` :

- `ai/openai/enabled` : activation de la fonctionnalité ;
- `ai/openai/api_key` : clé API conservée comme une chaîne ordinaire ;
- `ai/openai/model` : identifiant du modèle à employer lors de l’étape suivante ;
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

## Service et diagnostic

Le contrat `AiInsightService`, enregistré sous `ServiceId::AiInsight`, permet de
demander un rafraîchissement asynchrone et de lire son état. Les commandes
`ai.weather.refresh` et `ai.weather.status` offrent le même diagnostic pendant
la mise au point. Une demande forcée ignore le cache ; le futur flux d’analyse
pourra utiliser le rafraîchissement non forcé.
