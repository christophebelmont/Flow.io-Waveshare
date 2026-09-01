# SystemModule (`moduleId: system`)

## Rôle

Expose les commandes système transverses et surveille le bouton matériel de remise à zéro.

Type: module actif lorsqu'un bouton de remise à zéro est déclaré par le profil de carte.

## Dépendances

- `loghub`
- `cmd`
- `config`
- `eventbus`

## Services consommés

- `cmd` pour enregistrer les handlers
- `config` pour exposer la configuration système
- `eventbus` pour réagir aux changements de configuration
- `loghub` pour logging

## Services exposés

Aucun.

## Config / NVS

Module config: `system` (`moduleId = ConfigModuleId::System`, branche locale `1`)
- `lang` (`sys_lang`)
- `devicename` (`sys_dname`), défaut `flowio`, utilisé par l'interface web et les annonces DHCP/mDNS ; en DHCP, la valeur par défaut est annoncée sous la forme unique `FlowIO-XXXXXX`

## Commandes

- `system.ping`
  - réponse: `{"ok":true,"pong":true}`

- `system.reboot`
  - réponse ACK puis `esp_restart()`

- `system.factory_reset`
  - efface le namespace ConfigStore et les paramètres Wi-Fi persistants
  - répond par un ACK puis redémarre la carte

## Bouton matériel

Le profil Flow.io affecte GPIO4 au bouton de remise à zéro, actif bas avec pull-up
interne. Après débounce, un appui continu de 5 secondes exécute la même remise à
zéro que `system.factory_reset`, puis redémarre la carte. GPIO4 n'est pas exposé
comme binding I/O configurable.

## EventBus / DataStore / MQTT

- abonnement `ConfigChanged` pour normaliser la langue à chaud
