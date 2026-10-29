#include "gauge.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Cœur pur                                                             */
/* ------------------------------------------------------------------ */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void set_anchor(gauge_state_t *g, uint8_t a, float soc)
{
    g->anchor = a;
    g->soc_mah = soc;
    g->soc_at_anchor = soc;
    g->out_since = g->in_since = 0;
    g->known = 1;
    g->low_cnt = 0;
}

bool gauge_core_learn(gauge_state_t *g, float meas)
{
    if (!(meas >= GAUGE_CAP_MIN && meas <= GAUGE_CAP_MAX)) return false;
    if (g->n_learn >= 2 && fabsf(meas - g->cap_mah) > 0.5f * g->cap_mah) return false;   /* aberrant */
    float w = g->n_learn == 0 ? 1.0f : g->n_learn == 1 ? 0.5f : g->n_learn == 2 ? (1.0f / 3) : 0.25f;
    float ratio = g->soc_mah / g->cap_mah;
    g->cap_mah += w * (meas - g->cap_mah);
    g->soc_mah = ratio * g->cap_mah;      /* même pourcentage, nouvelle échelle */
    if (g->n_learn < 255) g->n_learn++;
    g->last_meas = meas;
    return true;
}

void gauge_core_boot(gauge_state_t *g, bool restored, bool power_on, float nominal,
                     int ocv_pct, uint32_t chg, uint32_t dchg, float k)
{
    if (!restored || g->magic != GAUGE_MAGIC || !(g->cap_mah >= GAUGE_CAP_MIN && g->cap_mah <= GAUGE_CAP_MAX)) {
        memset(g, 0, sizeof(*g));
        g->magic = GAUGE_MAGIC;
        g->cap_mah = nominal;
        g->soc_mah = nominal * clampf(ocv_pct, 0, 100) / 100.0f;
        g->chg_raw = chg;
        g->dchg_raw = dchg;
        return;
    }
    uint16_t mv_before = g->last_mv;
    uint8_t anchor_before = g->anchor;
    bool known_before = g->known;
    if (!gauge_core_count(g, chg, dchg, k)) {
        /* compteurs AXP remis à zéro (batterie débranchée) : on repart de la tension */
        g->soc_mah = g->cap_mah * clampf(ocv_pct, 0, 100) / 100.0f;
        g->known = 0;
        g->anchor = G_ANCHOR_NONE;
        return;
    }
    if (power_on && mv_before > 0 && mv_before < GAUGE_EMPTY_MV) {
        /* coupure batterie vide par l'AXP : ancre VIDE */
        if (known_before && anchor_before == G_ANCHOR_FULL) {
            gauge_core_learn(g, g->out_since - g->in_since);      /* tout ce qui est sorti depuis le plein */
        }
        set_anchor(g, G_ANCHOR_EMPTY, 0);
    }
}

bool gauge_core_count(gauge_state_t *g, uint32_t chg, uint32_t dchg, float k)
{
    if (chg < g->chg_raw || dchg < g->dchg_raw) {
        g->chg_raw = chg;
        g->dchg_raw = dchg;
        return false;
    }
    float in = (float)(chg - g->chg_raw) * k * GAUGE_CHG_EFF;
    float out = (float)(dchg - g->dchg_raw) * k;
    g->chg_raw = chg;
    g->dchg_raw = dchg;
    g->in_since += in;
    g->out_since += out;
    /* on tolère un dépassement (capacité sous-estimée) : l'ancre PLEIN corrigera */
    g->soc_mah = clampf(g->soc_mah + in - out, 0, g->cap_mah * 1.5f);
    return true;
}

bool gauge_core_step(gauge_state_t *g, float mv, float i_dchg_ma, bool vbus, bool charging)
{
    if (mv < 2500 || mv > 4500) return false;
    g->last_mv = (uint16_t)mv;

    if (vbus && !charging && mv > GAUGE_FULL_MV) {                       /* --- PLEIN --- */
        if (g->anchor == G_ANCHOR_FULL && g->known) {
            g->soc_mah = g->cap_mah;                    /* maintien sur secteur */
            g->out_since = g->in_since = 0;
            return false;
        }
        if (g->known && (g->anchor == G_ANCHOR_LOW || g->anchor == G_ANCHOR_EMPTY)) {
            /* charge depuis un point connu : la quantité stockée = capacité */
            gauge_core_learn(g, g->soc_at_anchor + g->in_since - g->out_since);
        }
        set_anchor(g, G_ANCHOR_FULL, g->cap_mah);
        return true;
    }

    if (!vbus && mv < GAUGE_LOW_MV && i_dchg_ma < GAUGE_LOW_MAX_MA) {     /* --- BAS --- */
        if (++g->low_cnt < 2) return false;                              /* confirmé 2 fois */
        if (g->anchor == G_ANCHOR_LOW && g->known) return false;
        if (g->known && g->anchor == G_ANCHOR_FULL) {
            gauge_core_learn(g, (g->out_since - g->in_since) / (1.0f - GAUGE_LOW_SOC));
        }
        set_anchor(g, G_ANCHOR_LOW, GAUGE_LOW_SOC * g->cap_mah);
        return true;
    }
    g->low_cnt = 0;
    return false;
}

int gauge_core_percent(const gauge_state_t *g, bool vbus, bool charging)
{
    float p = 100.0f * g->soc_mah / g->cap_mah;
    int pct = (int)(p + 0.5f);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    /* en charge : 100 % seulement à l'ancre PLEIN (comme un téléphone) */
    if (vbus && charging && pct > 99) pct = 99;
    if (vbus && !charging && g->anchor == G_ANCHOR_FULL) pct = 100;
    /* au-dessus de l'ancre BAS, ne jamais afficher 0 tant que l'AXP n'a pas coupé */
    if (pct < 1 && g->last_mv >= GAUGE_LOW_MV) pct = 1;
    return pct;
}

/* ------------------------------------------------------------------ */
/* Module : compteurs AXP + persistance NVS                             */
/* ------------------------------------------------------------------ */
#ifndef GAUGE_HOST_TEST
#include "axp202.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gauge";
#define SAVE_PERIOD_US (10LL * 60 * 1000000)

static gauge_state_t s_g;
static int64_t s_last_save_us;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "gauge", &s_g, sizeof(s_g));
    nvs_commit(h);
    nvs_close(h);
    s_last_save_us = esp_timer_get_time();
}

void gauge_init(float nominal_mah, int ocv_pct, bool power_on_reset)
{
    bool restored = false;
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_g);
        restored = nvs_get_blob(h, "gauge", &s_g, &n) == ESP_OK && n == sizeof(s_g);
        nvs_close(h);
    }
    uint32_t chg = 0, dchg = 0;
    axp_coulomb_raw(&chg, &dchg);
    uint8_t n_before = restored ? s_g.n_learn : 0;
    gauge_core_boot(&s_g, restored, power_on_reset, nominal_mah, ocv_pct, chg, dchg, axp_coulomb_mah_per_count());
    ESP_LOGI(TAG, "%s : capacité %.0f mAh (%u mesure%s), %.0f mAh restants, ancre %u%s",
             restored ? "restaurée" : "nouvelle", s_g.cap_mah, s_g.n_learn, s_g.n_learn > 1 ? "s" : "",
             s_g.soc_mah, s_g.anchor, s_g.known ? "" : " (estimée par la tension)");
    if (s_g.n_learn != n_before) ESP_LOGW(TAG, "capacité mesurée : %.0f mAh", s_g.last_meas);
    save();
}

void gauge_update(float mv, float i_dchg_ma, bool vbus, bool charging, int ocv_pct)
{
    uint32_t chg, dchg;
    if (axp_coulomb_raw(&chg, &dchg) != ESP_OK) return;
    if (!gauge_core_count(&s_g, chg, dchg, axp_coulomb_mah_per_count())) {
        ESP_LOGW(TAG, "compteurs AXP remis à zéro : réestimation par la tension");
        s_g.soc_mah = s_g.cap_mah * (ocv_pct < 0 ? 0 : ocv_pct > 100 ? 100 : ocv_pct) / 100.0f;
        s_g.known = 0;
        s_g.anchor = G_ANCHOR_NONE;
    }
    uint8_t n_before = s_g.n_learn;
    bool anchored = gauge_core_step(&s_g, mv, i_dchg_ma, vbus, charging);
    if (anchored) {
        ESP_LOGI(TAG, "ancre %u : %.0f mAh restants, capacité %.0f mAh", s_g.anchor, s_g.soc_mah, s_g.cap_mah);
    }
    if (s_g.n_learn != n_before) ESP_LOGW(TAG, "capacité mesurée %.0f mAh → retenue %.0f mAh", s_g.last_meas, s_g.cap_mah);
    if (anchored || esp_timer_get_time() - s_last_save_us >= SAVE_PERIOD_US) save();
}

int gauge_percent(bool vbus, bool charging) { return gauge_core_percent(&s_g, vbus, charging); }
const gauge_state_t *gauge_state(void) { return &s_g; }
#endif
