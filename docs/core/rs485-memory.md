# Mémoire RS485 et pilotes

Les buffers du transport et les 16 emplacements de l’ordonnanceur sont alloués une fois en PSRAM. Aucun buffer de trame n’est alloué par transaction. Les valeurs exactes sont journalisées au démarrage avec `sizeof(Storage)`.

Les pilotes sont contenus dans les slots PSRAM de `PoolDeviceModule`, avec des capacités bornées : six paliers, six points de débit et 1536 octets de configuration JSON par équipement. Le module indépendant de pompes variables a été supprimé.
