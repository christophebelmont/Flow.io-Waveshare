# Débordement de pile IO du 5 octobre 2026

Le journal à 13:51:53 indique `Stack canary watchpoint triggered (io)`.
L'ELF local correspond exactement à l'empreinte annoncée : SHA-256
`260c9e92a2a1bfd234a380b6b2cf250a7c64aa503fc770e44e5727acbc7c5980`.
Il a été conservé sous `/tmp/flowio-crash-260c9e92a.elf` avant recompilation.

La trace décodée suit `Module::taskEntry` → `IOModule::loop` → `ioTick_` →
`IOScheduler::tick` → `tickFastAds_` → `processAnalogDefinition_` →
`Log::debug` → `logVaModule` → `vsnprintf` → `_svfprintf_r` → `_dtoa_r` →
`__d2b` → `_Balloc` → `calloc` → allocateur ESP-IDF.

Le journal concerné est la trace `Calc` des mesures analogiques, qui formate
quatre valeurs flottantes. Le formatage est synchrone et consomme la pile de
l'appelant, même si l'expédition des journaux est ensuite asynchrone.
La présence de l'allocateur dans la trace ne signifie pas que le tas est épuisé :
le défaut signalé est le franchissement du garde de pile.

## Correction

La pile de la tâche `io` passe de 2 560 à 4 096 octets, soit 1 536 octets
supplémentaires en PSRAM pour absorber ce chemin de formatage. Les cadres
applicatifs de la trace ne contiennent pas de gros snapshot à éliminer.
La trace analogique reste disponible avec son format et sa cadence actuels.
La tâche `rs485` conserve sa pile indépendante de 3 072 octets.

Le minimum de 1 436 octets libres annoncé au démarrage a été relevé avant le
premier passage dans ce chemin ; il ne permet pas de dimensionner sa pointe.
La stabilité et la marge effective doivent être vérifiées sur carte, traces
analogiques activées, après plusieurs cycles d'acquisition.

## Vérification

- Compilation PlatformIO `Flowio-waveshare-esp32-s3` réussie dans le dossier
  isolé `/tmp/flowio-io-stack-build`.
- `git diff --check` réussi.
- Firmware exporté vers `binary/flowios3-2.0.1.bin` et paquet de mise à jour
  vers `binary/flowio-2.0.1.zip`.
- Aucun flash ni essai sur carte effectué dans cette intervention.
