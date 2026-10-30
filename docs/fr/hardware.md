# Notes matérielles — T-Watch 2020 V1

Français · [English](../en/hardware.md)

Référence : [schéma V1](https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library/tree/master/Schematic) de LilyGo.

## Broches

| Fonction | Broches |
|---|---|
| Écran ST7789 (SPI2) | MOSI 19, SCLK 18, CS 5, DC 27, rétroéclairage 12 (pas de reset, pas de ligne TE) |
| I2C 0 (capteurs) | SDA 21, SCL 22 — AXP202 `0x35`, PCF8563 `0x51`, BMA423 `0x19` |
| I2C 1 (tactile) | SDA 23, SCL 32 — FT6336 `0x38` |
| Interruptions | AXP202 35 (active basse, entrée seule), tactile 38, RTC 37, BMA423 39 |
| Vibreur | 4 (via un transistor) |
| LED infrarouge | 13 |
| Ampli I2S MAX98357 | BCLK 26, LRCLK 25, DIN 33 |

Le module ESP32 porte une flash W25Q128 de 16 Mo et une PSRAM IPS6404 de 8 Mo, toutes deux
alimentées par la sortie VDD_SDIO de l'ESP32. Il n'y a pas de quartz 32 kHz.

## Rails de l'AXP202

| Rail | Alimente | Firmware |
|---|---|---|
| DCDC3 | ESP32 3,3 V | toujours allumé (jamais coupé) |
| LDO1 | RTC 3,3 V | toujours allumé (non commutable) |
| LDO2 | écran (LCD_VDD) et rétroéclairage | 3,3 V, allumé |
| LDO3 | ampli MAX98357 seul | **coupé** ; allumé pendant un son avec démarrage progressif (voir plus bas) |
| LDO4 | VDD_1V8, simple point de test sur la V1 | 1,8 V, **gardé allumé** (certaines variantes de module ont une flash 1,8 V) |
| DCDC2 | non câblé sur la V1 (pas d'inductance) | coupé |
| EXTEN | — | coupé |

Autres réglages de l'AXP202 : tension de coupure (VOFF) 3,2 V, courant de charge 300 mA (le
minimum de la puce), LED de charge éteinte, compteur coulométrique actif.

**L'AXP202 garde ses registres tant que la batterie est branchée.** Un firmware précédent
qui a coupé un rail le laisse coupé pour le suivant : toujours régler les rails
explicitement au démarrage.

## Pièges rencontrés

- **Contrôleur Bluetooth d'ESP-IDF 5.5.3** : liaison chiffrée perdue en 0x208 juste après
  l'appairage. Utiliser ESP-IDF 5.5.6.
- **Confidentialité « host based » de NimBLE** : reconnexion impossible après la rotation de
  l'adresse privée de l'iPhone (~15 min, SMP timeout). La résolution d'adresse est laissée
  au contrôleur.
- **`BT_NIMBLE_GATT_CLIENT` dépend de `BT_NIMBLE_ROLE_CENTRAL`** dans ESP-IDF 5.5 :
  désactiver le rôle central supprime silencieusement `ble_gattc_*`, donc ANCS.
- **iOS ignore parfois la Security Request** : la montre l'envoie 500 ms après la connexion
  et coupe la liaison si le chiffrement n'a pas eu lieu 8 s plus tard, puis republie après
  3 s.
- **Publicité refusée après une connexion avortée** (`ENOMEM`, place de connexion pas encore
  libérée) : la publicité est relancée jusqu'à ce qu'elle passe ; sans cela la montre restait
  invisible jusqu'à un redémarrage du Bluetooth.
- **Collisions de paramètres de connexion** (0x22A) quand la montre demande les paramètres
  éco pendant qu'iOS change les siens : nouvel essai avec attente croissante jusqu'à
  acceptation.
- **Appel de courant de l'ampli audio** : passer LDO3 d'un coup de 0 à 3,3 V (condensateurs
  d'entrée de l'ampli) peut faire passer le système sous la coupure de l'AXP avec une
  batterie faible, et la montre s'éteint. LDO3 démarre à 1,3 V et monte par pas de 0,1 V. Les
  bips restent modérés pour limiter les pointes.
- **L'I2S mono de l'ESP32 ne remplit qu'un canal** : l'ampli peut écouter l'autre. Le son
  est envoyé en stéréo.
- **Annuler une mise à jour en plein envoi** : `httpd_stop()` attend le gestionnaire
  d'envoi, qui reçoit tant que le téléphone émet → montre figée. Redémarrage direct à la
  place.
- **L'image complète à 0x0 efface la NVS** (appairages). Pour une mise à jour USB, écrire
  l'app à `0x20000` avec `ota_data_initial.bin`.
- **Dalle à l'envers** sur certains lots : `board.h` → `BOARD_PANEL_ALT=1`.
- **Ligne TE du ST7789 non câblée** : pas de synchro anti-déchirement possible, d'où flash
  QIO 80 MHz, double tampon DMA de 60 lignes et animations courtes.

## Améliorations matérielles possibles (facultatives)

- Un oscillateur 32,768 kHz sur 32K_XN (GPIO 33 — partagé avec l'ampli, à faire avec soin)
  permettrait le light sleep avec une connexion BLE (`BTDM_CTRL_LPCLK_SEL_EXT_32K_XTAL`).
- La batterie (cellule générique 380 mAh de type « YX-W9A ») vieillit vite si elle est
  stockée vide ; l'écran *Énergie* affiche la capacité apprise pour savoir quand la
  changer.
