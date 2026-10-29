/*
 * power.h — politique d'énergie : PM/DFS, batterie, rétroéclairage, vibreur.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float vbat_mv;
    float i_dischg_ma;
    float i_chg_ma;
    bool  vbus;
    bool  charging;
    int   percent;
    float used_mah;        /* coulomb depuis débranchement (ou boot) */
    uint32_t since_s;      /* durée de la période de mesure */
    float avg_ma;          /* used_mah / heures, 0 si période trop courte */
    float off_avg_ma;      /* moyenne écran éteint (veille), 0 si < 2 min */
    float on_avg_ma;       /* moyenne écran allumé, 0 si < 10 s */
    float screen_pct;      /* part du temps écran allumé */
} power_telemetry_t;

esp_err_t power_init(void);           /* AXP, BMA423 suspend, PM, GPIO IRQ, LEDC, moteur */

/* Verrous PM (comptés). « active » = écran allumé : CPU max + pas de light sleep. */
void power_hold_active(bool hold);
void power_hold_nosleep(bool hold);

/* à appeler sur EV_AXP_IRQ depuis la tâche app ; retourne le masque d'IRQ */
uint64_t power_handle_irq(void);

/* rafraîchit l'estimation batterie (≈ toutes les 30–60 s) */
void power_update(void);
int  power_percent(void);
bool power_vbus(void);
bool power_charging(void);
void power_get_telemetry(power_telemetry_t *t);
/* appelé par display.c à chaque allumage/extinction (ventilation de la conso) */
void power_screen_segment(bool on);

/* Décharge précédente (historique NVS) et capacité apprise */
enum { POWER_HIST_RUNNING = 0, POWER_HIST_CHARGED = 1, POWER_HIST_EMPTY = 2, POWER_HIST_REBOOT = 3 };
typedef struct {
    uint32_t since_s;
    float    used_mah;
    float    avg_off_ma;     /* moyenne écran éteint */
    uint16_t vbat_mv;        /* dernière tension enregistrée */
    uint8_t  pct_start, pct_end;
    uint8_t  end;            /* POWER_HIST_* */
} power_hist_t;
bool     power_last_discharge(power_hist_t *out);
uint16_t power_capacity_mah(void);      /* apprise, sinon nominale */
bool     power_capacity_learned(void);
int      power_charge_start_pct(void); /* charge en cours : % de départ, sinon -1 */

/* rétroéclairage 0..255 (0 = éteint) */
void power_backlight(uint8_t level);

/* vibreur : motif = durées alternées ON/OFF en ms, terminé par 0 */
void power_vibrate(const uint16_t *pattern);
extern const uint16_t VIB_NOTIF[];
extern const uint16_t VIB_CALL[];
extern const uint16_t VIB_SHORT[];
