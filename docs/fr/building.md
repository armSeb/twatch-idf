# Compiler, flasher, tester

Français · [English](../en/building.md)

## Prérequis

- **ESP-IDF 5.5.6** — obligatoire. Avec ESP-IDF 5.5.3 (celui de la plateforme PlatformIO
  `espressif32@6.13.0`), le contrôleur Bluetooth de l'ESP32 (`libbtdm 045a658`) fait tomber
  la liaison iPhone en supervision timeout (0x208) juste après le chiffrement : l'appairage
  n'aboutit jamais. Le contrôleur fourni avec la 5.5.6 (`919c7f4`) corrige le problème. Au
  démarrage, le journal doit afficher `BTDM_INIT: BT controller compile version [919c7f4]`.
- LVGL **9.2.2**, récupéré par le gestionnaire de composants ESP-IDF
  (`main/idf_component.yml`).
- Python 3 et gcc pour les tests PC.

## Compiler

```bash
idf.py build                       # ESP-IDF 5.5.6
idf.py -p /dev/ttyUSB0 flash monitor
```

ou avec PlatformIO (qui impose ESP-IDF 5.5.6 via `platform_packages`) :

```bash
pio run -t upload -t monitor
```

Options, en variables d'environnement à la compilation :

| Variable | Effet |
|---|---|
| `TW_NO_TVOFF=1` | compile sans l'app *TV off* ni sa base de codes CC BY-SA 4.0 |
| `TW_EXTRA_COMPONENT_DIRS=/chemin` | dossier de composants supplémentaire (par exemple une copie locale de LVGL pour compiler hors ligne, avec `IDF_COMPONENT_MANAGER=0`) |
| `TW_DIAG_BLE_ONLY=1`, `TW_DIAG_BARE=1` | builds de diagnostic (BLE seul / sans périphériques) |

Après **toute modification de `sdkconfig.defaults`**, supprimer le `sdkconfig` généré
(garder `sdkconfig.defaults`) et recompiler ; sinon les anciennes valeurs restent.

## Organisation de la flash

| Adresse | Contenu |
|---|---|
| `0x1000` | bootloader (avec retour arrière) |
| `0x8000` | table de partitions |
| `0x9000` | NVS (réglages, appairages, historique batterie) |
| `0x10000` | données OTA (emplacement qui démarre) |
| `0x20000` | `ota_0` (3 Mo) |
| `0x320000` | `ota_1` (3 Mo) |
| `0x620000` | coredump |

## Flasher

**Première installation** (efface tout, appairages compris) :

```bash
esptool.py --chip esp32 -b 921600 write_flash 0x0 twatch2020v1-full.bin
```

**Par USB en gardant appairages et réglages** (ou migration depuis une version sans OTA) :

```bash
esptool.py --chip esp32 -b 921600 write_flash \
  0x1000 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 twatch2020v1-app.bin
```

`ota_data_initial.bin` fait démarrer la montre sur `ota_0`, où l'application est écrite.

**Sans fil** : voir le README (*Réglages › Mise à jour*). La page de mise à jour est servie
par la montre à `http://192.168.4.1` ; elle suit la langue du navigateur (français ou
anglais) et propose un bouton pour changer.

## Binaires publiés (CI)

`.github/workflows/build.yml` lance les tests PC et la compilation à chaque push. Sur une
étiquette `v*`, il publie :

- `twatch2020v1-full.bin` — image complète pour `0x0` ;
- `twatch2020v1-app.bin` — application seule (mise à jour Wi-Fi ou USB à `0x20000`) ;
- `twatch2020v1-app-notvoff.bin` — idem, compilée avec `TW_NO_TVOFF=1` (sans contenu CC BY-SA) ;
- `LICENSE` et `THIRD_PARTY_NOTICES.md`.

L'artefact du workflow contient aussi le bootloader, la table de partitions, les données OTA
et l'ELF (pour décoder les plantages).

## Tests PC

```bash
host_tests/run.sh
```

Lance, sous AddressSanitizer et UBSan :

- le parseur ANCS (10 000 fragmentations aléatoires du flux Data Source), le nettoyage UTF-8,
  le stockage des notifications (déduplication, tri, capacité) ;
- la jauge de batterie adaptative (batteries simulées de 150, 220 et 360 mAh, charges
  partielles, remises à zéro des compteurs) ;
- `check_i18n.py` : chaque chaîne `_()`/`N_()` a sa traduction anglaise, avec les mêmes
  spécificateurs printf dans le même ordre.

## Diagnostic

- **Plantage** : le coredump est conservé en flash.
  `esp-coredump info_corefile build/twatch.elf`.
- **BLE sans ordinateur** : *Réglages › À propos* affiche un journal BLE horodaté
  (connexions, adresse du pair, chiffrement, raisons de déconnexion, publicité), les
  compteurs de réveil, le tas, la PSRAM et les registres d'alimentation.
- **Batterie** : écran *Énergie* (courant moyen, part de veille, décharge précédente,
  capacité apprise).

## Polices

`main/fonts/` est généré par `tools/gen_fonts.sh` (il faut
[lv_font_conv](https://github.com/lvgl/lv_font_conv) et une copie de LVGL 9.2.2 pour les
sources Montserrat et Font Awesome) :

```bash
LVGL_DIR=/chemin/vers/lvgl tools/gen_fonts.sh
```

Pour ajouter une icône : ajouter son code Font Awesome aux lignes `font_text_16` et
`font_title_20`, régénérer, puis définir sa séquence UTF-8 dans `main/ui_kit.h`.
