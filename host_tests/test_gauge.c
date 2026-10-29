/*
 * Simulation de la jauge adaptative : batterie usée (capacité réelle
 * différente de la nominale), compteurs AXP quantifiés (0,364 mAh), cycles
 * charge / décharge, coupures AXP et redémarrages, charges partielles.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "gauge.h"

#define K 0.3641f                 /* mAh par pas de compteur AXP */

static const struct { float soc; float mv; } OCV[] = {
    {0.00f, 3300}, {0.05f, 3610}, {0.10f, 3690}, {0.20f, 3730}, {0.40f, 3800},
    {0.60f, 3870}, {0.80f, 4020}, {0.90f, 4110}, {1.00f, 4190},
};
static float ocv(float s)
{
    if (s <= 0) return 3300 - (0 - s) * 2000;
    for (int i = 1; i < (int)(sizeof(OCV) / sizeof(OCV[0])); i++)
        if (s <= OCV[i].soc) {
            float t = (s - OCV[i - 1].soc) / (OCV[i].soc - OCV[i - 1].soc);
            return OCV[i - 1].mv + t * (OCV[i].mv - OCV[i - 1].mv);
        }
    return 4190;
}

/* batterie simulée */
static float C_TRUE;              /* capacité réelle */
static float q;                   /* charge réelle (mAh) */
static double acc_in, acc_out;    /* accumulateurs AXP (mAh) */
static uint32_t cnt_in, cnt_out;
static gauge_state_t g, saved;

static void axp_tick(float in_mah, float out_mah)
{
    acc_in += in_mah; acc_out += out_mah;
    while (acc_in >= K) { acc_in -= K; cnt_in++; }
    while (acc_out >= K) { acc_out -= K; cnt_out++; }
}

static int pct(bool vbus, bool chg) { return gauge_core_percent(&g, vbus, chg); }

/* une minute de fonctionnement ; renvoie false si l'AXP coupe (3,2 V) */
static bool minute_on_battery(float ma)
{
    float d = ma / 60.0f;
    q -= d;
    axp_tick(0, d);
    float mv = ocv(q / C_TRUE) - ma * 0.25f;   /* chute sous charge (R ≈ 0,25 Ω) */
    if (mv < 3200) return false;
    gauge_core_count(&g, cnt_in, cnt_out, K);
    gauge_core_step(&g, mv, ma, false, false);
    return true;
}

static void charge_to_full(void)
{
    for (int m = 0; m < 600; m++) {
        float ma = q < 0.9f * C_TRUE ? 150 : 25;       /* CC puis CV */
        bool done = q >= C_TRUE - 0.5f;
        if (!done) { float d = ma / 60.0f; if (q + d > C_TRUE) d = C_TRUE - q; q += d; axp_tick(d, 0); }
        float mv = done ? 4180 : ocv(q / C_TRUE) + 30;
        gauge_core_count(&g, cnt_in, cnt_out, K);
        gauge_core_step(&g, mv, 0, true, !done);
        if (done) {
            int p = pct(true, false);
            if (p != 100) { printf("ÉCHEC : fin de charge affichée %d %%\n", p); assert(0); }
            return;
        }
        int p = pct(true, true);
        assert(p <= 99);
    }
    assert(!"charge sans fin");
}

static void reboot(bool power_on)
{
    saved = g;                    /* NVS */
    g = saved;
    float mv = ocv(q / C_TRUE);
    gauge_core_boot(&g, true, power_on, 380, 50, cnt_in, cnt_out, K);
    (void)mv;
}

static void run(float c_true, float start_q, int cycles)
{
    C_TRUE = c_true; q = start_q; acc_in = acc_out = 0; cnt_in = 1000; cnt_out = 5000;
    memset(&g, 0, sizeof(g));
    gauge_core_boot(&g, false, true, 380, (int)(100 * start_q / c_true), cnt_in, cnt_out, K);
    printf("\n=== batterie réelle %.0f mAh, départ %.0f %% ===\n", c_true, 100 * start_q / c_true);
    for (int c = 0; c < cycles; c++) {
        charge_to_full();
        int minutes = 0, max_err = 0;
        bool rebooted = false;
        while (minute_on_battery(c % 2 ? 9.0f : 7.5f)) {
            minutes++;
            if (!rebooted && minutes == 300) { reboot(false); rebooted = true; }   /* OTA en cours de route */
            int p_true = (int)(100 * q / C_TRUE + 0.5f), p = pct(false, false);
            if (g.n_learn >= 1 && abs(p - p_true) > max_err) max_err = abs(p - p_true);
        }
        reboot(true);             /* coupure AXP puis rebranchement */
        printf("cycle %d : %4d min, capacité retenue %.0f mAh (%u mesures, dernière %.0f), "
               "écart max %% affiché après apprentissage : %d pts\n",
               c + 1, minutes, g.cap_mah, g.n_learn, g.last_meas, max_err);
    }
    float err = fabsf(g.cap_mah - c_true) / c_true;
    printf("→ capacité finale %.0f mAh (réelle %.0f) : erreur %.1f %%\n", g.cap_mah, c_true, 100 * err);
    assert(err < 0.08f);
}

int main(void)
{
    run(150, 30, 4);     /* batterie très usée, nominale 380 */
    run(360, 300, 3);    /* batterie presque neuve */
    run(220, 10, 3);
    /* charge partielle depuis 60 % : pas d'apprentissage (départ non ancré) */
    C_TRUE = 200; q = 120; cnt_in = 10; cnt_out = 10;
    memset(&g, 0, sizeof(g));
    gauge_core_boot(&g, false, true, 380, 60, cnt_in, cnt_out, K);
    charge_to_full();
    assert(g.n_learn == 0);
    printf("\ncharge partielle non ancrée : capacité inchangée (%.0f) — OK\n", g.cap_mah);
    /* compteurs AXP remis à zéro : détection */
    assert(!gauge_core_count(&g, 0, 0, K));
    printf("remise à zéro des compteurs détectée — OK\n");
    printf("\nJAUGE : TOUS LES TESTS PASSENT\n");
    return 0;
}
