/*
 * remote_cfg.h — télécommandes infrarouges décrites en JSON (gardé en NVS,
 * modifiable depuis la page web de la montre).
 *
 * {
 *   "remotes": [
 *     { "name": "TV Philips", "proto": "rc6", "addr": "00", "cols": 2,
 *       "keys": [
 *         { "label": "Veille", "code": "0C", "color": "red" },
 *         { "label": "Vol +",  "code": "10", "repeat": true },
 *         { "label": "" },                      ← case vide
 *         { "label": "Menu", "code": "54", "w": 2 },    ← sur deux colonnes
 *         { "label": "HDMI", "proto": "rc5", "addr": "00", "code": "3F" },
 *         { "label": "OFF" }                    ← code à chercher
 *       ] } ] }
 *
 * proto : nec, samsung, rc5, rc6, sony12, sony15, sony20. addr et code en
 * hexadécimal ; pour nec, une adresse de 2 chiffres est l'adresse standard
 * (suivie de son inverse), 4 chiffres l'adresse 16 bits telle qu'émise.
 * cols : 1 à 4 colonnes. color : green, red, blue, yellow, orange, purple, grey.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ir_proto.h"

#define RC_MAX_REMOTES 8
#define RC_MAX_KEYS    30
#define RC_NAME_LEN    24
#define RC_LABEL_LEN   14

typedef enum { RC_GREY, RC_GREEN, RC_RED, RC_BLUE, RC_YELLOW, RC_ORANGE, RC_PURPLE, RC_COLOR_N } rc_color_t;

typedef struct {
    char     label[RC_LABEL_LEN];   /* vide = case vide */
    uint8_t  color, w;              /* w : colonnes occupées */
    bool     repeat;                /* répète tant qu'on garde le doigt */
    bool     own;                   /* protocole / adresse propres à la touche */
    bool     has;                   /* code connu */
    uint8_t  proto;
    uint16_t addr, cmd;
} rc_key_t;

typedef struct {
    char     name[RC_NAME_LEN];
    int8_t   proto;                 /* -1 : à découvrir */
    bool     has_addr;
    uint16_t addr;
    uint8_t  cols;
    uint8_t  nkeys;
    rc_key_t keys[RC_MAX_KEYS];
} rc_remote_t;

typedef struct {
    uint8_t     n;
    rc_remote_t r[RC_MAX_REMOTES];
} rc_set_t;

/* code effectif d'une touche (protocole et adresse hérités de la télécommande) */
bool rc_key_code(const rc_remote_t *r, const rc_key_t *k, ir_code_t *out);

/* JSON ↔ structures. parse : 0 si correct, sinon renvoie un code d'erreur
 * court (« json », « remotes », « proto »…) dans err. */
int    rc_parse(const char *json, rc_set_t *set, const char **err);
char  *rc_to_json(const rc_set_t *set, bool pretty);    /* à libérer avec free() */

/* modèles prêts à l'emploi */
#define RC_PRESET_N 6
const char *rc_preset_name(int i);
const char *rc_preset_json(int i);                        /* un objet « remote » */
int         rc_add_preset(rc_set_t *set, int i);          /* 0 si ajouté */

const char *rc_color_name(int c);
rc_set_t   *rc_alloc(void);                              /* jeu vide, en PSRAM ; free() */

/* NVS ; load crée les modèles par défaut (et reprend les codes de l'ancienne
 * app Lampe IR) si rien n'est encore enregistré */
void rc_load(rc_set_t *set);
int  rc_save(const rc_set_t *set);
int  rc_save_json(const char *json, const char **err);   /* page web */
char *rc_load_json(bool pretty);                           /* page web, à libérer */
