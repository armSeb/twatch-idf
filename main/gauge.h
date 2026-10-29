/*
 * gauge.h — jauge de batterie adaptative (comptage coulométrique + capacité
 * apprise), à la manière des jauges de téléphone.
 *
 * Principe :
 *  - l'AXP202 tient deux compteurs monotones (charge entrée / charge sortie).
 *    Ils survivent aux redémarrages de l'ESP32 et à une coupure batterie vide
 *    (l'AXP reste alimenté par la batterie) : on compte donc sans perte en
 *    comparant avec les valeurs enregistrées en NVS ;
 *  - trois ancres recalent l'état de charge :
 *      PLEIN  fin de charge (USB, charge terminée, > 4,10 V)        → 100 %
 *      BAS    tension < 3,45 V sous faible courant, 2 fois de suite →   4 %
 *      VIDE   démarrage après coupure AXP (dernière tension < 3,55 V) → 0 %
 *  - entre deux ancres connues, l'écart compté donne une mesure de capacité
 *    (PLEIN→BAS, PLEIN→VIDE, BAS/VIDE→PLEIN). La capacité retenue converge
 *    par moyenne pondérée (1re mesure : remplace la nominale, puis 1/2, 1/3,
 *    puis 1/4), mesures aberrantes (±50 %) rejetées après 2 cycles.
 *
 * Le cœur (gauge_core_*) est pur : testé sur PC (host_tests).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { G_ANCHOR_NONE = 0, G_ANCHOR_FULL, G_ANCHOR_LOW, G_ANCHOR_EMPTY };

#define GAUGE_MAGIC        0x47415532u   /* « GAU2 » */
#define GAUGE_FULL_MV      4100
#define GAUGE_LOW_MV       3450
#define GAUGE_LOW_SOC      0.04f
#define GAUGE_LOW_MAX_MA   40.0f
#define GAUGE_EMPTY_MV     3550
#define GAUGE_CHG_EFF      0.99f         /* rendement coulombique LiPo */
#define GAUGE_CAP_MIN      40.0f
#define GAUGE_CAP_MAX      600.0f

typedef struct {
    uint32_t magic;
    float    cap_mah;      /* capacité retenue (apprise ou nominale) */
    float    soc_mah;      /* charge restante estimée */
    float    out_since;    /* mAh sortis depuis la dernière ancre */
    float    in_since;     /* mAh entrés (× rendement) depuis la dernière ancre */
    float    soc_at_anchor;
    uint32_t chg_raw, dchg_raw;   /* compteurs AXP lors du dernier comptage */
    uint16_t last_mv;
    uint8_t  n_learn;      /* nombre de mesures de capacité retenues */
    uint8_t  anchor;       /* G_ANCHOR_* : dernière ancre */
    uint8_t  known;        /* état de charge fiable (au moins une ancre) */
    uint8_t  low_cnt;
    float    last_meas;    /* dernière mesure de capacité (diagnostic) */
} gauge_state_t;

/* ---- cœur pur ---- */
/* démarrage : état restauré valide ou non ; ocv_pct = estimation par tension */
void  gauge_core_boot(gauge_state_t *g, bool restored, bool power_on, float nominal_mah,
                      int ocv_pct, uint32_t chg, uint32_t dchg, float mah_per_count);
/* comptage depuis les compteurs AXP ; renvoie false si les compteurs ont été remis à zéro */
bool  gauge_core_count(gauge_state_t *g, uint32_t chg, uint32_t dchg, float mah_per_count);
/* ancres et apprentissage ; renvoie true si une ancre a été posée (à sauvegarder) */
bool  gauge_core_step(gauge_state_t *g, float mv, float i_dchg_ma, bool vbus, bool charging);
int   gauge_core_percent(const gauge_state_t *g, bool vbus, bool charging);
bool  gauge_core_learn(gauge_state_t *g, float meas_mah);

/* ---- module (AXP + NVS) ---- */
void  gauge_init(float nominal_mah, int ocv_pct, bool power_on_reset);
void  gauge_update(float mv, float i_dchg_ma, bool vbus, bool charging, int ocv_pct);
int   gauge_percent(bool vbus, bool charging);
const gauge_state_t *gauge_state(void);
