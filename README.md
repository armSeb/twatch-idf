# twatch-idf — firmware T-Watch 2020 V1 (ESP-IDF + LVGL 9 + NimBLE)

Firmware écrit de zéro pour la **LilyGo T-Watch 2020 V1**, sans Arduino : notifications
iPhone via **ANCS**, heure synchronisée via **CTS**, interface tactile LVGL, cadran
analogique « montre ancienne » à complications. Priorités : stabilité, autonomie, finition.

| Cadrans | Écrans |
|---|---|
| ![cadrans](docs/cadrans.png) ![pub](docs/cadran_pub.png) | ![écrans](docs/ecrans.png) |

*(rendus PC de l'UI réelle avec des données factices)*

## Fonctionnalités

- Notifications iOS (ANCS) : liste triée, détail, actions iOS (*Effacer*, *Répondre*,
  *Accepter*/*Refuser* un appel), suppression synchronisée dans les deux sens, vibreur.
- Écran d'appel entrant dédié.
- **Lecture en cours** (Apple Media Service) : titre, artiste, album, progression, volume et
  commandes lecture/pause/précédent/suivant/volume, pour toute app audio iOS (Spotify,
  Musique, Deezer, podcasts…). AMS ne transmet pas la pochette.
- Heure et fuseau (DST compris) récupérés de l'iPhone (Current Time Service), RTC PCF8563
  en UTC comme référence.
- Trois cadrans : **Rétro** (cadran ivoire, aiguilles bleuies, réserve de marche =
  batterie, guichet date, petite seconde), **Pub** (carré plein écran, vert bouteille et laiton, harpe, jauge FUEL,
  enseigne OPEN/CLOSED pour la liaison iPhone) et **Numérique**.
- Appairage sécurisé LE Secure Connections + code à 6 chiffres, reconnexion automatique.
- Mode avion (Bluetooth coupé → vrai light sleep), écran **Énergie** (consommation moyenne
  mesurée par le compteur coulométrique de l'AXP202).
- **Apps** : lanceur (← depuis le cadran) avec **Chrono** (tours), **Minuteur** (bipe et vibre même
  écran éteint, l'app se rouvre), **Lampe** (blanc / rouge) et **TV-B-Gone** (éteint la plupart des téléviseurs par la LED infrarouge,
codes Europe / USA). Un chrono ou un minuteur
  en marche garde la main : l'écran s'éteint dessus et le réveil y revient. Un petit cadre d'applications
  (`app.h`) permet d'en ajouter en un fichier.
- **Contrôles rapides** (↓ depuis le cadran) : Bluetooth, **Ne pas déranger**, Lampe,
  Lecture, luminosité.
- Interface en **français ou en anglais** (Réglages › Affichage › Langue), changement
  immédiat sans redémarrage.
- Démarrage quasi instantané, coredump en flash en cas de crash.

Consommation mesurée : ~8,6 mA de moyenne **en utilisation réelle** (3 % du temps écran
allumé, notifications iPhone connectées), soit ≈ 44 h sur la batterie de 380 mAh d'origine.
Voir [Autonomie](#autonomie).

---

## Flasher un binaire

Binaires fournis par la CI (onglet *Releases* ou *Actions*).

**Mise à jour sans câble (OTA Wi-Fi)** — le cas normal :

1. Montre : *Réglages › Mise à jour › Démarrer*. Le Bluetooth se coupe, la montre crée le
   réseau Wi-Fi **T-Watch-MAJ** et affiche son mot de passe en gros chiffres (tiré au sort
   une fois, puis toujours le même : l'iPhone se reconnecte tout seul ensuite).
2. iPhone ou PC : se connecter à ce réseau, ouvrir **http://192.168.4.1**, choisir
   `twatch2020v1-app.bin`, *Envoyer*.
3. La montre vérifie l'image (firmware T-Watch, SHA-256), l'active et redémarre.

L'image est écrite dans l'autre emplacement (`ota_0`/`ota_1`) : l'ancien firmware reste
intact jusqu'au bout. Au premier démarrage, le nouveau est « en essai » : s'il plante avant
30 s de fonctionnement, le bootloader revient automatiquement à l'ancien. Refus sous 30 %
de batterie hors USB ; abandon automatique après 5 min sans envoi.

**Première installation** (efface tout, y compris les appairages) :

```bash
esptool.py --chip esp32 -b 921600 write_flash 0x0 twatch2020v1-full.bin
```

**Par USB en gardant les appairages** (ou migration depuis une version sans OTA) :

```bash
esptool.py --chip esp32 -b 921600 write_flash \
  0x1000 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 twatch2020v1-app.bin
```

(`ota_data_initial.bin` remet le démarrage sur `ota_0`, là où le `.bin` est écrit.)

## Compiler

```bash
pio run -t upload -t monitor        # PlatformIO
# ou
idf.py build flash monitor          # ESP-IDF 5.5.6
```

- **ESP-IDF 5.5.6 obligatoire** (forcé via `platform_packages` dans `platformio.ini`).
  Avec ESP-IDF 5.5.3 (celui de `espressif32@6.13.0`), le contrôleur BT de l'ESP32
  (`libbtdm 045a658`) fait mourir la liaison iPhone en supervision timeout (0x208) juste
  après le chiffrement : appairage impossible. Le contrôleur d'IDF 5.5.6 (`919c7f4`)
  corrige le problème. Vérifier au boot : `BTDM_INIT: BT controller compile version [919c7f4]`.
- LVGL 9.2.2 est récupéré par le gestionnaire de composants ESP-IDF (`main/idf_component.yml`).
- Après toute modif de `sdkconfig.defaults` : supprimer `sdkconfig*` (sauf `.defaults`) puis rebuild.
- Coredump : `esp-coredump info_corefile build/twatch.elf` après un crash.
- Polices : `tools/gen_fonts.sh` (lv_font_conv).

## Appairage iPhone

1. Montre : glisser ← → *Réglages* → *Bluetooth* → **Appairer un iPhone**.
2. iPhone : *Réglages › Bluetooth* → **T-Watch**.
3. Saisir sur l'iPhone le **code à 6 chiffres affiché sur la montre**.
4. Accepter « Autoriser les notifications » (la montre réessaie pendant ~1 min).

Si l'iPhone oublie la montre, elle efface son propre bond et relance un appairage propre.

## Utilisation

| Où | Geste / bouton | Effet |
|---|---|---|
| Cadran | ↑ | Notifications (refermer : ↓ ou →) |
| Cadran | ↓ | Contrôles rapides (refermer : ↑ ou →) |
| Cadran | ← | Apps : Lecture, Chrono, Minuteur, Lampe, Énergie, Réglages |
| Cadran | appui long | Choix du cadran (Affichage) |
| Partout ailleurs | → ou « ‹ » | Retour à l'écran d'où l'on vient |
| Écran éteint | double tap | Allume l'écran (comme le bouton) |
| Bouton (écran éteint) | appui court | Allume l'écran (sur la dernière notif non vue en mode discret) |
| Bouton (hors cadran) | appui court | Retour cadran |
| Bouton (sur cadran) | appui court | Éteint l'écran |
| Bouton | 4 s | Coupure matérielle AXP202 |

**Ne pas déranger** (contrôles rapides) : ni vibration ni allumage pour les notifications,
qui attendent dans la liste ; les appels sonnent toujours. Une lune le signale sur le cadran.

![apps](docs/apps.png)

Réglages (← puis *Réglages*) : un accueil par catégories (dont **Mise à jour**) avec résumé de l'état, puis
**Affichage** (cadran Rétro/Pub/Digital, luminosité, extinction), **Alertes** (vibreur,
allumage sur notif — sinon mode discret —, double tap), **Bluetooth** (mode avion,
appairage, « Oublier les appareils » derrière une confirmation), **Énergie**, **À propos**.
Retour par l'en-tête « ‹ » ou en glissant vers la droite.

![réglages](docs/reglages.png)

---

## Autonomie

**Sur ESP32 « classique », le light sleep est impossible tant que le contrôleur BLE est
actif** sans quartz 32 kHz externe (absent de la T-Watch 2020) : le contrôleur garde un
verrou `ESP_PM_NO_LIGHT_SLEEP`. C'est vrai quel que soit le framework. Le firmware joue donc
sur tout le reste :

| Levier | Réglage |
|---|---|
| DFS | CPU à **10 MHz** au repos (le contrôleur BT relâche son verrou de fréquence pendant le modem sleep) ; **240 MHz** seulement pendant une interaction (toucher, animation), retour au minimum après 1,5 s d'inactivité même écran allumé |
| BLE | modem sleep + un seul passage en paramètres éco après la synchro ANCS : 150–300 ms, latence 4, timeout 6 s (règles Apple respectées) |
| Publicité | 160 ms pendant 30 s après déconnexion, puis ~1 s |
| Tâches | aucun timer périodique écran éteint : la tâche app dort jusqu'à un événement (max 60 s) ; tick LVGL lu à la demande |
| AXP202 | ADC limités à V/I batterie, LED de charge éteinte |
| Périphériques | BMA423 en suspend, PCF8563 CLKOUT coupé, ST7789 en SLPIN, FT6336 en mode monitor, GPIO inutilisés figés |
| Mode avion | contrôleur BT arrêté → **light sleep automatique** |

Écran **Énergie** : moyenne réelle depuis le dernier débranchement USB (« Moy. x mA, reste ~y h »),
part de veille, décharge précédente (historique en NVS, survit à une batterie vide).

**Jauge adaptative** (`gauge.c`, testée sur PC) : comptage coulométrique sur les compteurs
monotones de l'AXP202 (jamais remis à zéro, ils survivent aux redémarrages et à la coupure),
recalé sur trois ancres — fin de charge = 100 %, < 3,45 V sous faible courant = 4 %,
coupure AXP (3,2 V) = 0 %. Chaque intervalle entre deux ancres donne une mesure de capacité ;
la capacité retenue converge par moyenne pondérée (la 1re remplace les 380 mAh nominaux),
mesures aberrantes rejetées. Le pourcentage et le « reste ~h » suivent la batterie réelle,
même usée.

Piste matérielle (non retenue) : un oscillateur 32,768 kHz sur 32K_XN (GPIO33) permettrait
le light sleep BLE connecté (`BTDM_CTRL_LPCLK_SEL_EXT_32K_XTAL` + `RTC_CLK_SRC_EXT_OSC`).

## Architecture

```
            ┌───────────── tâche hôte NimBLE (cœur 0) ─────────────┐
            │ ble.c   GAP, sécurité passkey, service batterie,       │
 iPhone ◄──►│         params éco, publicité                          │
            │ ancs.c  découverte ANCS+CTS, file de commandes,        │
            │         réassemblage Data Source (ancs_parse.c)        │
            └──────────┬───────────────────────────┬──────────────────┘
                       │ g_notif_queue (copie)     │ app_post(événement)
            ┌──────────▼───────────────────────────▼──────────────────┐
 AXP IRQ ──►│ tâche app (cœur 1) : seule propriétaire de LVGL          │
 (GPIO35)   │ main.c  boucle événements / lv_timer_handler / veille    │
            │ ui.c, face_classic.c   notif_store.c  tri+dédup+UTF-8    │
            │ display.c (ST7789 + FT6336)  power.c (AXP, DFS, vibreur) │
            └──────────────────────────────────────────────────────────┘
```

Règles de stabilité :

- **Un seul propriétaire par état** : LVGL et notifications → tâche app ; état ANCS →
  tâche hôte NimBLE. Échanges par files, pas de mutex LVGL.
- **Une seule procédure ATT en vol** côté ANCS, numéro de génération sur chaque callback.
- **Data Source réassemblé octet par octet**, testé sur PC (10 000 fragmentations aléatoires, ASan/UBSan).
- **Replay iOS** filtré par `EventFlagPreExisting` + déduplication par contenu (FNV-1a, TTL 5 min).
- **Texte iOS nettoyé** (UTF-8 tronqué, émojis, ZWJ…) avant LVGL.
- **IRQ AXP en niveau** : aucune IRQ perdue pendant un light sleep.

Bibliothèques : pas d'`esp_lvgl_port` (timer de tick + tâche dédiée = réveils permanents),
pas de lib AXP/BMA/FT (quelques registres suffisent), `esp_lcd` natif, `i2c_master` d'IDF 5.

## Pièges rencontrés

- **Contrôleur BT d'ESP-IDF 5.5.3** : liaison chiffrée perdue en 0x208 (voir *Compiler*).
- **Confidentialité « host based » de NimBLE** : reconnexion impossible après rotation de
  l'adresse privée de l'iPhone (~15 min, SMP timeout). Résolution laissée au contrôleur.
- **En cours d'investigation** : après des heures de fonctionnement, l'iPhone n'arrive
  parfois plus à se reconnecter ; couper/relancer le Bluetooth de la montre ne suffit pas,
  un redémarrage complet si. Pour le diagnostic sans PC : *À propos* affiche un journal BLE
  horodaté (connexions, adresse vue, chiffrement, déconnexions, publicité) et l'écran
  *Énergie* signale une publicité annoncée mais inactive.
- **`BT_NIMBLE_GATT_CLIENT` dépend de `BT_NIMBLE_ROLE_CENTRAL`** dans IDF 5.5 : désactiver le
  rôle central supprime silencieusement `ble_gattc_*`, donc ANCS.
- **Rails AXP202** : sur cette montre LDO3 alimente l'écran et LDO4 le tactile. Aucun rail
  n'est coupé. L'AXP garde ses registres tant que la batterie est branchée : un ancien
  firmware qui coupe un rail « contamine » les suivants.
- **Image complète à 0x0** : efface la NVS (appairages). Mettre à jour avec l'app seule à 0x10000.
- Dalle à l'envers sur certaines séries : `board.h` → `BOARD_PANEL_ALT=1`.
- Le signal TE du ST7789 n'est pas câblé : pas de synchro anti-tearing possible, d'où
  flash QIO 80 MHz, double tampon DMA de 60 lignes et animations courtes.

## Écrire une app

Une app est un fichier `main/app_xxx.c` qui définit un `const app_t` (nom, icône, couleur,
`create(scr)`, et au besoin `destroy`, `on_show`, `on_hide`, `on_event`,
`screen_timeout_ms`), puis une ligne dans `main/apps.c`. Le système lui fournit un écran vide
où le geste → ramène déjà à l'écran d'origine ; `ui_kit.h` donne la palette, les icônes et
les briques (`ui_label`, `ui_button`, `ui_header`, `ui_toast`…).

La vue est **détruite à l'extinction de l'écran** (RAM) : l'état durable vit dans des
variables statiques, jamais dans les objets LVGL. Pour agir écran éteint (minuteur), l'app
arme un `esp_timer` et appelle `app_post_event(&app_xxx, code)` ; si `on_event` renvoie
`true`, le système rouvre l'app et allume l'écran. Voir `app_chrono.c`, `app_timer.c`,
`app_lamp.c`.

## Traduire

Mécanisme à la gettext : la chaîne française sert de clé, `_("Réglages")` dans le code,
`N_()` dans les tableaux statiques (traduits à l'affichage). La table anglaise est dans
`main/i18n.c`. `host_tests/check_i18n.py` (lancé par `run.sh`) signale toute chaîne sans
traduction et tout format printf qui diffère entre les deux langues.

## Tests PC

```bash
host_tests/run.sh
```

Parseur ANCS, nettoyage UTF-8, stockage (dédup/tri/capacité), jauge, sous `-fsanitize=address,undefined` ; couverture des traductions.

## Fichiers

| Fichier | Rôle |
|---|---|
| `main/main.c` | `app_main`, tâche app, boucle d'événements, veille écran |
| `main/ble.c` | NimBLE : GAP, sécurité, publicité, params éco, service batterie, mode avion |
| `main/ancs.c` / `ancs_parse.c` | client ANCS + CTS + AMS (`media.h`) ; réassemblage Data Source |
| `main/notif_store.c` | stockage, tri par date, dédup, nettoyage UTF-8 |
| `main/ui.c` / `face_classic.c` / `face_pub.c` | écrans LVGL, navigation ; cadrans analogiques |
| `main/display.c` | ST7789 (`esp_lcd`, SPI DMA), FT6336, glue LVGL, DFS écran allumé |
| `main/power.c` / `axp202.c` | PMIC, batterie (OCV + compensation IR), verrous PM, rétroéclairage, vibreur |
| `main/wclock.c` | PCF8563 (UTC), fuseau, resynchro de l'horloge système |
| `main/settings.c` | réglages NVS |
| `main/app.h` / `apps.c` / `app_*.c` / `ui_kit.h` | cadre d'apps, registre, apps livrées, briques d'UI partagées |
| `main/i18n.c` | traduction FR → EN |
| `main/gauge.c` | jauge adaptative (compteur coulométrique, apprentissage de la capacité) |
| `main/ota.c` | mise à jour Wi-Fi : point d'accès, page web, écriture OTA, validation / retour arrière |
| `main/fonts/` | polices générées (`tools/gen_fonts.sh`) |

## Pistes

- Réveil au poignet / double tape (config BMA423 à charger, ~150–500 µA).
- Page web de mise à jour en anglais.
- Répondre aux SMS : impossible via ANCS (seules les actions iOS positive/négative existent).

---

## Crédits

| Projet | Usage | Licence |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) — Espressif | framework, drivers, `esp_lcd`, PM | Apache-2.0 |
| [Apache NimBLE](https://github.com/apache/mynewt-nimble) (via ESP-IDF) | pile BLE hôte | Apache-2.0 |
| [LVGL](https://github.com/lvgl/lvgl) 9.2 | interface graphique | MIT |
| [lv_font_conv](https://github.com/lvgl/lv_font_conv) | conversion des polices | MIT |
| [Montserrat](https://github.com/JulietaUla/Montserrat) — Julieta Ulanovsky | polices de texte (`main/fonts/`) | SIL OFL 1.1 |
| [Font Awesome Free](https://fontawesome.com) | icônes (`main/fonts/`) | SIL OFL 1.1 (polices) |
| [TTGO_TWatch_Library](https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library) — LilyGo | référence brochage et périphériques | MIT |
| [My-TTGO-Watch](https://github.com/sharandac/My-TTGO-Watch) — sharandac | inspiration (gestion d'énergie, BLE) ; aucun code repris | GPL-3.0 |
| [TV-B-Gone](https://www.tvbgone.com) — Mitch Altman, Limor Fried ; port Arduino Ken Shirriff ; [composant ESP-IDF](https://github.com/pedrominatel/esp-components/tree/main/tv-b-gone) Pedro Minatel | base de codes infrarouges et émission RMT (`components/tvbgone/`, adapté : voir `NOTICE.md`) | CC BY-SA 4.0 |
| Apple — *ANCS Specification*, *Accessory Design Guidelines* | protocole ANCS, paramètres de connexion | — |

Code du firmware sous licence MIT (voir `LICENSE`), sauf `components/tvbgone/` qui reste sous
CC BY-SA 4.0.
