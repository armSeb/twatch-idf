# TV-B-Gone (composant tiers, CC BY-SA 4.0)

Ce dossier est une **adaptation** du composant ESP-IDF
[pedrominatel/esp-components › tv-b-gone](https://github.com/pedrominatel/esp-components/tree/main/tv-b-gone)
(révision `4f3b5870bb4af0599f4aa0a19cd443f53123bd49`, version 0.3.0), lui-même fondé sur le
TV-B-Gone de **Mitch Altman** et **Limor Fried** (base de codes WORLDcodes, port Arduino de
**Ken Shirriff**). Il reste sous licence **CC BY-SA 4.0** (voir `LICENSE`), indépendamment de
la licence MIT du reste du projet.

Modifications apportées pour la T-Watch :

- `WORLDcodes.cpp` → `WORLDcodes.c`, toutes les tables en `const` : la base de codes reste en
  flash au lieu d'occuper ~25 Ko de DRAM ;
- signatures et structure `IrCode` passées en pointeurs `const` en conséquence ;
- tampon de symboles RMT (5 Ko) alloué à `tvbgone_core_init()` et libéré à
  `tvbgone_core_deinit()` au lieu d'être statique ;
- journal par code ramené au niveau DEBUG.
- `README.md` : liens vers les exemples (non inclus ici) pointés sur le dépôt d'origine.
