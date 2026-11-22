/* ir_proto.c — voir ir_proto.h */
#include "ir_proto.h"
#include <string.h>

static const char *const NAMES[IR_PROTO_N] = { "nec", "samsung", "rc5", "rc6", "sony12", "sony15", "sony20" };

const char *ir_proto_name(int p) { return p >= 0 && p < IR_PROTO_N ? NAMES[p] : "?"; }

int ir_proto_parse(const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < IR_PROTO_N; i++) if (!strcmp(s, NAMES[i])) return i;
    if (!strcmp(s, "sony")) return IR_SONY12;
    return -1;
}

int ir_proto_carrier(int p) { return p == IR_RC5 || p == IR_RC6 ? 36000 : p >= IR_SONY12 ? 40000 : 38000; }
int ir_cmd_count(int p) { return p == IR_RC5 || p >= IR_SONY12 ? 128 : 256; }
int ir_frame_ms(int p) { return p == IR_NEC || p == IR_SAMSUNG ? 70 : p == IR_RC5 ? 25 : p == IR_RC6 ? 24 : 135; }

/* ------------------------------------------------------------------ assemblage */
typedef struct { uint16_t *us; int n, max; bool mark; } buf_t;   /* mark : niveau de la dernière durée */

/* ajoute une durée au niveau donné, fusionnée avec la précédente si même niveau */
static void put(buf_t *b, bool mark, int us)
{
    if (b->n < 0) return;
    if (b->n == 0 && !mark) return;                      /* un message commence par une marque */
    if (b->n > 0 && b->mark == mark) {
        int v = b->us[b->n - 1] + us;
        b->us[b->n - 1] = (uint16_t)(v > 65535 ? 65535 : v);
        return;
    }
    if (b->n >= b->max) { b->n = -1; return; }
    b->us[b->n++] = (uint16_t)us;
    b->mark = mark;
}

static void pulse_distance(buf_t *b, uint32_t v, int nbits, int mark, int s0, int s1)
{
    for (int i = 0; i < nbits; i++) { put(b, true, mark); put(b, false, (v >> i) & 1 ? s1 : s0); }
}

/* Manchester : RC5 « 1 » = espace puis marque ; RC6 « 1 » = marque puis espace */
static void manchester(buf_t *b, int bit, int half, bool rc6)
{
    bool first_mark = rc6 ? bit : !bit;
    put(b, first_mark, half);
    put(b, !first_mark, half);
}

int ir_encode(const ir_code_t *c, bool toggle, uint16_t *us, int max)
{
    buf_t b = { us, 0, max, false };
    switch (c->proto) {
    case IR_NEC:
        put(&b, true, 9000); put(&b, false, 4500);
        pulse_distance(&b, c->addr | ((uint32_t)(c->cmd & 0xFF) << 16) | ((uint32_t)(uint8_t)~c->cmd << 24), 32, 562, 562, 1687);
        put(&b, true, 562); put(&b, false, 10000);
        break;
    case IR_SAMSUNG: {
        uint8_t a = (uint8_t)c->addr, k = (uint8_t)c->cmd;
        put(&b, true, 4500); put(&b, false, 4500);
        pulse_distance(&b, a | ((uint32_t)a << 8) | ((uint32_t)k << 16) | ((uint32_t)(uint8_t)~k << 24), 32, 560, 560, 1690);
        put(&b, true, 560); put(&b, false, 10000);
        break;
    }
    case IR_RC5: {
        /* S1 = 1, S2 = bit 6 de la commande inversé (RC5 étendu), bascule,
         * 5 bits d'adresse puis 6 bits de commande, poids fort d'abord */
        uint32_t w = (1u << 13) | ((uint32_t)!((c->cmd >> 6) & 1) << 12) | ((uint32_t)toggle << 11)
                   | ((uint32_t)(c->addr & 0x1F) << 6) | (c->cmd & 0x3F);
        for (int i = 13; i >= 0; i--) manchester(&b, (w >> i) & 1, 889, false);
        put(&b, false, 10000);
        break;
    }
    case IR_RC6: {
        const int t = 444;
        put(&b, true, 6 * t); put(&b, false, 2 * t);    /* en-tête */
        manchester(&b, 1, t, true);                      /* bit de départ */
        for (int i = 0; i < 3; i++) manchester(&b, 0, t, true);   /* mode 0 */
        manchester(&b, toggle, 2 * t, true);             /* bascule, double largeur */
        uint32_t w = ((uint32_t)(c->addr & 0xFF) << 8) | (c->cmd & 0xFF);
        for (int i = 15; i >= 0; i--) manchester(&b, (w >> i) & 1, t, true);
        put(&b, false, 6 * t + 2000);
        break;
    }
    case IR_SONY12: case IR_SONY15: case IR_SONY20: {
        int abits = c->proto == IR_SONY12 ? 5 : 8;
        uint32_t w = (c->cmd & 0x7F) | ((uint32_t)(c->addr & ((1u << abits) - 1)) << 7);
        int nbits = c->proto == IR_SONY12 ? 12 : c->proto == IR_SONY15 ? 15 : 20;
        if (c->proto == IR_SONY20) w = (c->cmd & 0x7F) | ((uint32_t)(c->addr & 0x1F) << 7) | ((uint32_t)(c->addr >> 5 & 0xFF) << 12);
        for (int r = 0; r < 3; r++) {                    /* trois fois, toutes les 45 ms */
            int len = 2400 + 600;
            put(&b, true, 2400); put(&b, false, 600);
            for (int i = 0; i < nbits; i++) {
                int m = (w >> i) & 1 ? 1200 : 600;
                put(&b, true, m); put(&b, false, 600);
                len += m + 600;
            }
            put(&b, false, 45000 - len > 0 ? 45000 - len : 600);
        }
        break;
    }
    default:
        return 0;
    }
    return b.n < 0 ? 0 : b.n;
}
