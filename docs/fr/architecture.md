# Architecture, énergie et jauge de batterie

Français · [English](../en/architecture.md)

## Tâches et propriétaires

```
            ┌──────────── tâche hôte NimBLE (cœur 0) ─────────────┐
            │ ble.c   GAP, sécurité passkey, service batterie,      │
 iPhone ◄──►│         paramètres de connexion, publicité, journal   │
            │ ancs.c  découverte ANCS + CTS + AMS, file de commandes│
            │         réassemblage Data Source (ancs_parse.c)       │
            └──────────┬───────────────────────────┬────────────────┘
                       │ g_notif_queue (copies)    │ app_post(événement)
            ┌──────────▼───────────────────────────▼────────────────┐
 IRQ AXP ──►│ tâche app (cœur 1) : seule propriétaire de LVGL       │
 INT tactile│ main.c   boucle d'événements / lv_timer_handler / veille│
 esp_timers │ ui.c, face_*.c, app_*.c   notif_store.c               │
            │ display.c (ST7789 + FT6336)  power.c, gauge.c, sound.c│
            └───────────────────────────────────────────────────────┘
```

Règles de stabilité :

- **Un seul propriétaire par état.** LVGL et le stockage des notifications appartiennent à
  la tâche app ; l'état ANCS à la tâche hôte NimBLE. Elles échangent par files et
  événements, jamais par un mutex LVGL partagé.
- **Une seule procédure ATT en vol** côté ANCS, avec un numéro de génération vérifié dans
  chaque callback (les réponses tardives après une reconnexion sont ignorées).
- **Data Source réassemblé octet par octet**, testé sur PC avec des fragmentations
  aléatoires.
- **Rejeu iOS** filtré par `EventFlagPreExisting` et déduplication par contenu (FNV-1a, durée
  de vie 5 minutes) ; seules les 32 notifications les plus récentes sont gardées.
- **Texte iOS nettoyé** (UTF-8 tronqué, émojis, séquences ZWJ…) avant d'arriver à LVGL.
- **Interruption AXP en niveau** : aucune interruption perdue pendant un light sleep.
- **Événements de partout, interface à un seul endroit** : timers, BLE et interruptions ne
  font que poster des événements ; `app_post_event()` permet à une app de réagir écran
  fermé.

Pas d'`esp_lvgl_port` (son timer de tick et sa tâche réveillent le CPU en permanence), pas de
bibliothèque AXP/BMA/FT (quelques registres suffisent), `esp_lcd` natif, `i2c_master`
d'ESP-IDF 5.

## Interface

- **Navigation par origine** : chaque écran retient d'où il a été ouvert ; glisser → ou
  l'en-tête « ‹ » y ramène toujours.
- **Écrans paresseux** : réglages, lecture en cours, lanceur, contrôles rapides et app
  hébergée sont construits à la demande et détruits à l'extinction de l'écran ; la RAM ne
  contient que ce qui est affiché. Écrans permanents : cadran, liste des notifications,
  détail, appel, énergie, appairage, à propos.
- **Apps** (`app.h`) : voir [apps-and-i18n.md](apps-and-i18n.md). L'app ouverte reste
  construite écran éteint (elle reçoit `on_hide()`, puis `on_show()` au réveil) et la montre
  se réveille dessus.
- **Bannière de notification** : hors du cadran, une nouvelle notification glisse sur la
  couche supérieure au lieu de changer d'écran ; la toucher ouvre le détail, qui ramène
  exactement à l'écran d'avant. Les appels prennent toujours tout l'écran et y ramènent
  ensuite.
- **Analyseur Wi-Fi** (`app_wifi.c`) : le pilote Wi-Fi n'existe que tant que l'app est
  visible écran allumé (`esp_wifi_init` dans `on_show`, `esp_wifi_deinit` dans `on_hide`) ;
  chaque relève allume la radio, fait un balayage actif (~1,2 s) puis la coupe 4 s. Le BLE
  reste connecté (coexistence logicielle).
- **SDR** (`app_sdr.c`, `sdr_rx.c`, `sdr_mode.c`) : le MAC Wi-Fi de l'ESP32 sait recopier
  les échantillons bruts du convertisseur de réception (I/Q, 10 bits, ici 40 MS/s) dans deux
  bancs de SRAM fixes (0x3FFE8000–0x3FFF8000). Ces 64 Ko sont réservés à l'édition de liens
  et rendus au tas au démarrage en mode normal : rien n'est perdu. Ouvrir l'app redémarre la
  montre en « mode SDR » (drapeau en RAM RTC, consommé au démarrage : un plantage ramène au
  mode normal) : le Bluetooth ne démarre pas et la mémoire de son contrôleur est libérée, les
  bancs restent à la radio, l'app s'ouvre d'office. Une tâche sur le cœur 0 accorde des
  canaux normaux (1/4/7/10/13 pour la vue bande, un seul canal pour le zoom), capture 4096
  échantillons, retire la composante continue, fait des FFT fenêtrées de Hann (16 × 256 ou
  8 × 512 points, moyennées) et produit des lignes de 240 colonnes en dBFS ; l'interface les
  dessine dans deux canevas RGB565 en PSRAM avec un plancher de bruit suivi automatiquement.
  Quitter l'app redémarre en mode normal ; écran éteint, la radio est complètement arrêtée.
- **WebRX** (`app_webrx.c`, `webrx.c`, `webrx_cfg.c`) : client OpenWebRX / OpenWebRX+. Les
  réseaux Wi-Fi (jusqu'à 3) et l'adresse du serveur se saisissent sur une page servie par la
  montre (`/webrx`, même point d'accès et même serveur web que la mise à jour ; la session se
  termine par un redémarrage qui rouvre l'app). `webrx.c` fait tourner une tâche sur le cœur 0 :
  Wi-Fi station (réseaux connus par ordre de préférence, modem sleep), `wss://` par
  `esp_transport` (TLS avec le paquet de racines d'ESP-IDF, tampons mbedTLS en PSRAM ;
  `utilisateur:mot_de_passe@` dans l'adresse devient une authentification HTTP Basic), poignée
  de main OpenWebRX, `connectionproperties` (audio à 12 kHz pour tous les modes),
  `selectprofile` et `dspcontrol`. La liste `modes` du serveur (modes ajoutés compris) donne
  les modes écoutables et leur bande passante ; les modes numériques passent par leur mode
  sous-jacent. Les trames audio (IMA-ADPCM avec mots de synchronisation) sont décodées dans un
  tampon de 2,7 s en PSRAM ; une seconde tâche sur le cœur 1 garde ~0,4 s d'avance et
  alimente l'ampli I2S, en sautant ou doublant un échantillon par bloc de 20 ms pour suivre
  l'horloge du serveur. Les trames FFT donnent la mini-waterfall, seulement écran allumé. Le
  CPU est plafonné à 160 MHz (80 MHz écran éteint) tant que la radio tourne, et le Bluetooth
  n'est suspendu que si le pilote Wi-Fi manque de RAM interne.
- **Changer de langue** reconstruit tous les écrans sur place, sans redémarrer.
- Les objets LVGL sont alloués sur le tas ; les allocations de plus de 4 Ko vont en PSRAM, la
  RAM interne est gardée pour les piles, les tampons DMA et la radio.

## Énergie

Sur un ESP32 « classique », **le light sleep est impossible tant que le contrôleur BLE est
actif** sans quartz 32 kHz externe (absent de la T-Watch 2020) : le contrôleur garde un
verrou `ESP_PM_NO_LIGHT_SLEEP`, quel que soit le framework. Tout le reste est donc réglé :

| Levier | Réglage |
|---|---|
| Fréquence | CPU à **10 MHz** au repos (le contrôleur BT relâche son verrou de fréquence en modem sleep) ; **240 MHz** seulement pendant une interaction, retour au minimum 1,5 s après le dernier toucher même écran allumé ; pleine vitesse pendant une mise à jour Wi-Fi |
| Liaison BLE | modem sleep ; une fois ANCS prêt, demande de paramètres économes (intervalle 150–300 ms, latence 4, timeout 6 s, dans les règles Apple), **redemandés jusqu'à acceptation** avec une attente croissante (iOS entre souvent en collision avec ses propres mises à jour, erreur 0x22A) |
| Publicité | 160 ms pendant 30 s après une déconnexion, puis ~1 s ; nouvel essai chaque seconde si la pile la refuse |
| Tâches | aucun timer périodique écran éteint ; la tâche app dort jusqu'à un événement (60 s max) ; tick LVGL lu à la demande |
| Écran | ST7789 en veille, rétroéclairage coupé, tactile en mode monitor avec un détecteur de double tap interrogé seulement après une interruption tactile |
| AXP202 | ADC limités à la tension et au courant batterie, LED de charge éteinte ; rails inutilisés coupés ; le rail du haut-parleur n'est alimenté que pendant un son |
| Périphériques | BMA423 en suspend, CLKOUT du PCF8563 coupé, GPIO inutilisés à un niveau défini |
| Mode avion | contrôleur BT arrêté → **light sleep automatique** |

Résultat typique : environ **7,7 mA en veille**, iPhone connecté et notifications actives.

## Jauge de batterie adaptative

`gauge.c` (cœur pur testé sur PC) compte la charge avec les compteurs coulométriques de
l'AXP202 (0,364 mAh par impulsion). Ces compteurs sont monotones et jamais remis à zéro : ils
survivent aux redémarrages et aux coupures. Le comptage est recalé sur trois ancres :

| Ancre | Condition | Valeur |
|---|---|---|
| PLEINE | sur USB, charge terminée, > 4,10 V | 100 % |
| BASSE | < 3,45 V sous faible courant, vu deux fois | 4 % |
| VIDE | démarrage après une dernière tension enregistrée < 3,55 V | 0 % |

Chaque intervalle entre deux ancres donne une mesure de capacité. La capacité retenue
converge avec des poids 1, ½, ⅓ puis ¼ (la première mesure remplace les 380 mAh nominaux) ;
les mesures aberrantes au-delà de ±50 % sont rejetées dès qu'il y en a deux. Le pourcentage
et l'autonomie restante suivent donc la batterie réelle, même usée. En charge, l'affichage
plafonne à 99 % ; l'état est enregistré en NVS toutes les 10 minutes et à chaque ancre.

La capacité apprise est la capacité *utilisable par la montre* : une cellule âgée, à forte
résistance interne, atteint la tension de coupure sous les pointes de courant plus tôt
qu'en test sur banc.

## Mise à jour Wi-Fi (OTA)

- La montre coupe le Bluetooth, démarre un point d'accès WPA2 (`T-Watch-OTA`, mot de passe
  tiré une fois et gardé en NVS), un petit serveur DNS qui répond 192.168.4.1 à tout, et un
  serveur HTTP. Les sondes de connectivité Apple et Android reçoivent une réponse « succès »
  pour que le téléphone reste sur le réseau.
- L'envoi est écrit au fil de l'eau par blocs de 4 Ko dans l'emplacement inactif. Le premier
  bloc est vérifié (signature d'image ESP32 et descripteur d'application au nom `twatch`),
  l'image complète est vérifiée (SHA-256) avant activation.
- Retour arrière : une nouvelle image démarre « à vérifier » et est validée après 30 s de
  fonctionnement ; un plantage avant cela fait revenir à l'emplacement précédent.
- Annuler redémarre la montre (retour propre au Bluetooth). Pendant un envoi, le redémarrage
  est immédiat : arrêter le serveur HTTP attendrait la fin du gestionnaire, qui reçoit
  toujours.
- *Revenir à la x.y.z* vérifie entièrement l'autre emplacement avant d'y démarrer.

## Fichiers sources

| Fichier | Rôle |
|---|---|
| `main/main.c` | `app_main`, tâche app, boucle d'événements, veille écran |
| `main/ble.c` | NimBLE : GAP, sécurité, publicité, paramètres de connexion, service batterie, mode avion, journal |
| `main/ancs.c` / `ancs_parse.c` | client ANCS + CTS + AMS (`media.h`) ; réassemblage Data Source |
| `main/notif_store.c` | stockage, tri par date, déduplication, nettoyage UTF-8 |
| `main/ui.c`, `ui_kit.h` | écrans, navigation, briques d'interface partagées |
| `main/face_classic.c`, `face_pub.c` | cadrans analogiques (dessinés en code) |
| `main/app.h`, `apps.c`, `app_*.c` | cadre d'apps, registre, apps livrées |
| `main/display.c` | ST7789 (`esp_lcd`, SPI DMA), FT6336, glue LVGL, double tap, fréquence CPU |
| `main/power.c`, `axp202.c`, `gauge.c` | PMIC, rails, batterie, verrous PM, rétroéclairage, vibreur, jauge adaptative |
| `main/sound.c` | bips sur l'ampli I2S |
| `main/wclock.c` | PCF8563 (UTC), fuseau, resynchro de l'horloge système |
| `main/settings.c` | réglages en NVS |
| `main/i18n.c` | table français → anglais |
| `main/ota.c` | mise à jour Wi-Fi : point d'accès, page web, écriture OTA, validation, retour arrière |
| `components/tvbgone/` | codes infrarouges et émetteur RMT (CC BY-SA 4.0) |
