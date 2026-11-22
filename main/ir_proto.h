/*
 * ir_proto.h — codage des protocoles infrarouges courants en durées
 * d'impulsions (sans matériel : testable sur PC).
 *
 *   NEC      38 kHz  en-tête 9 / 4,5 ms, 32 bits (adresse 16 bits telle
 *                    qu'émise, commande, commande inversée)
 *   Samsung  38 kHz  en-tête 4,5 / 4,5 ms, adresse 8 bits émise deux fois
 *   RC5      36 kHz  Philips, Manchester 889 µs, adresse 5 bits, commande 7
 *   RC6      36 kHz  Philips mode 0, Manchester 444 µs, adresse et commande 8
 *   Sony     40 kHz  SIRC 12, 15 ou 20 bits, trame envoyée 3 fois
 *
 * RC5 et RC6 ont un bit de bascule : il change à chaque nouvel appui, pas
 * quand on garde la touche enfoncée (sinon l'appareil voit un seul appui).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { IR_NEC, IR_SAMSUNG, IR_RC5, IR_RC6, IR_SONY12, IR_SONY15, IR_SONY20, IR_PROTO_N } ir_proto_t;

typedef struct {
    uint8_t  proto;              /* ir_proto_t */
    uint16_t addr;               /* NEC : 16 bits tels qu'émis (voir ir_nec_std) */
    uint16_t cmd;
} ir_code_t;

static inline uint16_t ir_nec_std(uint8_t a) { return (uint16_t)(a | ((uint8_t)~a << 8)); }
static inline bool ir_nec_is_std(uint16_t a) { return (uint8_t)(a >> 8) == (uint8_t)~a; }

const char *ir_proto_name(int p);           /* "nec", "samsung", "rc5", "rc6", "sony12"… */
int         ir_proto_parse(const char *s);  /* -1 si inconnu */
int         ir_proto_carrier(int p);        /* Hz */
int         ir_cmd_count(int p);            /* nombre de commandes possibles (128 ou 256) */
int         ir_frame_ms(int p);             /* durée d'envoi d'une trame, répétitions comprises */

/* Durées alternées marque / espace en µs, en commençant par une marque.
 * Renvoie le nombre de durées (0 si trop long pour `max`). */
int ir_encode(const ir_code_t *c, bool toggle, uint16_t *us, int max);
