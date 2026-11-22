/* Tests PC du codage infrarouge (ir_proto.c) : chaque trame est décodée par
 * un décodeur indépendant qui doit retrouver adresse, commande et bascule. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ir_proto.h"

static uint16_t us[400];
static int n;

static int near(int v, int ref) { return abs(v - ref) <= ref / 4 + 60; }

/* NEC / Samsung : distance d'impulsion */
static uint32_t dec_pd(int hdr_m, int hdr_s)
{
    assert(near(us[0], hdr_m) && near(us[1], hdr_s));
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) {
        assert(near(us[2 + 2 * i], 560));
        int s = us[3 + 2 * i];
        assert(near(s, 560) || near(s, 1690));
        if (near(s, 1690)) v |= 1u << i;
    }
    return v;
}

/* Manchester : reconstitue la suite de demi-périodes (1 = marque) */
static int halves(int start, int unit, uint8_t *h, int max)
{
    int k = 0;
    for (int i = start; i < n; i++) {
        bool mark = !(i & 1);
        int q = (us[i] + unit / 2) / unit;
        if (q > 8) break;                                  /* silence final */
        for (int j = 0; j < q && k < max; j++) h[k++] = mark;
    }
    return k;
}

int main(void)
{
    /* NEC standard et étendu */
    ir_code_t c = { IR_NEC, ir_nec_std(0x00), 0x45 };
    n = ir_encode(&c, false, us, 400);
    uint32_t v = dec_pd(9000, 4500);
    assert((v & 0xFFFF) == 0xFF00 && ((v >> 16) & 0xFF) == 0x45 && (v >> 24) == 0xBA);
    c = (ir_code_t){ IR_NEC, 0xEF00, 0x12 };
    n = ir_encode(&c, false, us, 400);
    v = dec_pd(9000, 4500);
    assert((v & 0xFFFF) == 0xEF00 && ((v >> 16) & 0xFF) == 0x12);

    /* Samsung : adresse deux fois */
    c = (ir_code_t){ IR_SAMSUNG, 0x07, 0x02 };
    n = ir_encode(&c, false, us, 400);
    v = dec_pd(4500, 4500);
    assert((v & 0xFF) == 7 && ((v >> 8) & 0xFF) == 7 && ((v >> 16) & 0xFF) == 2 && (v >> 24) == 0xFD);

    /* RC5 : 14 bits Manchester de 889 µs, « 1 » = espace puis marque */
    for (int tg = 0; tg < 2; tg++) {
        c = (ir_code_t){ IR_RC5, 0x00, 0x0C };
        n = ir_encode(&c, tg, us, 400);
        uint8_t h[64] = { 0 };                             /* demi-périodes manquantes = espace */
        h[0] = 0;                                          /* demi-période initiale (espace implicite) */
        int k = 1 + halves(0, 889, h + 1, 63);
        assert(k >= 27);
        uint32_t w = 0;
        for (int i = 0; i < 14; i++) {
            assert(h[2 * i] != h[2 * i + 1]);
            w = (w << 1) | (h[2 * i] == 0);
        }
        assert((w >> 13) == 1 && ((w >> 12) & 1) == 1 && ((w >> 11) & 1) == (uint32_t)tg);
        assert(((w >> 6) & 0x1F) == 0 && (w & 0x3F) == 0x0C);
    }

    /* RC6 mode 0 : en-tête 6t/2t, départ 1, mode 000, bascule double, 16 bits */
    for (int tg = 0; tg < 2; tg++) {
        c = (ir_code_t){ IR_RC6, 0x00, 0x10 };
        n = ir_encode(&c, tg, us, 400);
        assert(near(us[0], 2664) && us[1] >= 800);
        uint8_t h[128] = { 0 };
        /* l'espace de l'en-tête peut avoir absorbé la première demi-période d'espace */
        int k = halves(1, 444, h, 128);
        int i = 2;                                         /* saute les 2 demi-périodes d'espace de l'en-tête */
        assert(h[i] == 1 && h[i + 1] == 0); i += 2;        /* départ = 1 */
        for (int m = 0; m < 3; m++) { assert(h[i] == 0 && h[i + 1] == 1); i += 2; }
        int t = h[i];
        assert(h[i + 1] == t && h[i + 2] != t && h[i + 3] != t); i += 4;
        assert(t == tg);
        uint32_t w = 0;
        for (int b = 0; b < 16; b++) { assert(h[i] != h[i + 1]); w = (w << 1) | h[i]; i += 2; }
        assert(i <= k + 1);
        assert((w >> 8) == 0x00 && (w & 0xFF) == 0x10);
    }

    /* Sony 12 bits, 3 trames */
    c = (ir_code_t){ IR_SONY12, 0x01, 0x15 };
    n = ir_encode(&c, false, us, 400);
    assert(near(us[0], 2400));
    uint32_t w = 0;
    for (int i = 0; i < 12; i++) if (near(us[2 + 2 * i], 1200)) w |= 1u << i;
    assert((w & 0x7F) == 0x15 && (w >> 7) == 1);
    int frames = 0;
    for (int i = 0; i < n; i += 2) frames += near(us[i], 2400);
    assert(frames == 3);

    /* noms */
    for (int p = 0; p < IR_PROTO_N; p++) assert(ir_proto_parse(ir_proto_name(p)) == p);
    assert(ir_proto_parse("sony") == IR_SONY12 && ir_proto_parse("x") == -1);
    printf("IR : OK (NEC, Samsung, RC5, RC6, Sony)\n");
    return 0;
}
