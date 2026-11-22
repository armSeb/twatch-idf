/* banc d'essai de motion.c : faux BMA423, faux NVS (fichier), dates pilotées ; deux démarrages */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "motion.h"
#include "bma423.h"
#include "settings.h"
#include "wclock.h"
#include "nvs.h"
#include "esp_system.h"
#include "driver/i2c_master.h"

int64_t g_now;
static uint32_t H[7];
static settings_t S;
settings_t *settings_get(void) { return &S; }
/* date */
static int Y = 2026, Mo = 11, Dd = 16; static bool TV = true;
bool wclock_time_valid(void) { return TV; }
void wclock_local_tm(struct tm *t) { memset(t, 0, sizeof *t); t->tm_year = Y - 1900; t->tm_mon = Mo - 1; t->tm_mday = Dd; t->tm_hour = 12; mktime(t); }
/* NVS en mémoire */
static uint32_t N_day, N_today, N_hist[7]; static bool N_has; static int N_writes;
esp_err_t nvs_open(const char *n, int m, nvs_handle_t *h) { *h = 1; return (m == NVS_READONLY && !N_has) ? 0x1102 : 0; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { *v = !strcmp(k, "day") ? N_day : N_today; return 0; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { if (!strcmp(k, "day")) N_day = v; else N_today = v; return 0; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *b, size_t *n) { memcpy(b, N_hist, *n); return 0; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *b, size_t n) { memcpy(N_hist, b, n); return 0; }
esp_err_t nvs_commit(nvs_handle_t h)
{
    N_has = true; N_writes++;
    FILE *f = fopen("nvs.bin", "wb"); fwrite(&N_day, 4, 1, f); fwrite(&N_today, 4, 1, f); fwrite(N_hist, 4, 7, f); fclose(f);
    return 0;
}
void nvs_close(nvs_handle_t h) {}
static shutdown_handler_t SH;
int esp_register_shutdown_handler(shutdown_handler_t h) { SH = h; return 0; }
/* faux capteur */
static uint32_t CNT; static bool STEP_EN, WRIST_EN, ACC; static int LOADS, RESETS;
/* la vraie puce n'acquitte pas toujours la commande de reset : erreur de bus */
int8_t bma4_soft_reset(struct bma4_dev *d) { CNT = 0; STEP_EN = WRIST_EN = false; return BMA4_E_COM_FAIL; }
int8_t bma423_init(struct bma4_dev *d) { d->chip_id = 0x13; return 0; }
int8_t bma423_write_config_file(struct bma4_dev *d) { LOADS++; return 0; }
int8_t bma423_set_remap_axes(const struct bma423_axes_remap *r, struct bma4_dev *d) { return 0; }
int8_t bma4_set_accel_config(const struct bma4_accel_config *c, struct bma4_dev *d) { return 0; }
int8_t bma4_set_int_pin_config(const struct bma4_int_pin_config *c, uint8_t l, struct bma4_dev *d) { return 0; }
int8_t bma4_set_interrupt_mode(uint8_t m, struct bma4_dev *d) { return 0; }
int8_t bma423_feature_enable(uint8_t f, uint8_t en, struct bma4_dev *d)
{ if (f & BMA423_STEP_CNTR) STEP_EN = en; if (f & BMA423_WRIST_WEAR) WRIST_EN = en; return 0; }
int8_t bma423_map_interrupt(uint8_t l, uint16_t m, uint8_t en, struct bma4_dev *d) { return 0; }
int8_t bma4_read_accel_xyz(struct bma4_accel *a, struct bma4_dev *d) { a->x = 50; a->y = -300; a->z = -900; return 0; }
int8_t bma423_read_int_status(uint16_t *s, struct bma4_dev *d) { *s = WRIST_EN ? BMA423_WRIST_WEAR_INT : 0; return 0; }
int8_t bma4_set_advance_power_save(uint8_t e, struct bma4_dev *d) { return 0; }
int8_t bma4_set_accel_enable(uint8_t e, struct bma4_dev *d) { ACC = e; return 0; }
int8_t bma423_reset_step_counter(struct bma4_dev *d) { CNT = 0; RESETS++; return 0; }
int8_t bma423_step_counter_output(uint32_t *v, struct bma4_dev *d) { *v = CNT; return 0; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int t) { return 0; }
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t d, i2c_master_transmit_multi_buffer_info_t *b, size_t n, int t) { return 0; }

static void walk(uint32_t n) { if (STEP_EN && ACC) CNT += n; }
static uint32_t today(void) { g_now += 3000000; return motion_steps_today(); }

int main(int argc, char **argv)
{
    if (argc > 1) {                  /* 2e démarrage : NVS relu du fichier */
        FILE *f = fopen("nvs.bin", "rb"); assert(f);
        assert(fread(&N_day, 4, 1, f) == 1 && fread(&N_today, 4, 1, f) == 1 && fread(N_hist, 4, 7, f) == 7); fclose(f);
        N_has = true; Dd = 17;
        S.step_count = S.wrist_wake = true;
        motion_init();
        assert(LOADS == 1 && today() == 42);
        goto phase2;
    }
    remove("nvs.bin");
    /* rien d'activé : pas de microcode */
    motion_init();
    assert(LOADS == 0 && !motion_ok() && !motion_steps_on());
    /* pas activés */
    S.step_count = true; motion_apply();
    assert(LOADS == 1 && STEP_EN && !WRIST_EN && ACC);
    walk(1200); assert(today() == 1200);
    /* 15 min : sauvegarde */
    g_now += 16LL * 60 * 1000000; motion_tick(); assert(N_today == 1200 && N_day == 20261116);
    walk(300); assert(today() == 1500);
    /* désactivation : total figé et sauvé */
    S.step_count = false; motion_apply(); assert(!ACC && N_today == 1500 && today() == 1500);
    walk(999);                       /* capteur coupé : rien */
    S.step_count = true; S.wrist_wake = true; motion_apply();
    assert(LOADS == 1 && WRIST_EN && today() == 1500);
    walk(500); assert(today() == 2000);
    assert(motion_on_irq());
    /* minuit */
    Dd = 17; motion_tick();
    motion_history(H);
    assert(H[0] == 2000 && today() == 0 && N_day == 20261117 && N_hist[0] == 2000);
    walk(42);
    /* redémarrage (arrêt propre) : sauvegarde puis reprise */
    SH(); assert(N_today == 42);
    printf("1er démarrage OK\n");
    return 0;
phase2:
    walk(8); assert(today() == 50);
    /* trois jours sans porter la montre */
    uint32_t *h = H; Dd = 20; motion_tick(); motion_history(h);
    printf("historique : %u %u %u %u %u\n", h[0], h[1], h[2], h[3], h[4]);
    assert(h[0] == 0 && h[1] == 0 && h[2] == 50 && h[3] == 2000 && today() == 0);
    /* heure inconnue : on compte, pas de bascule */
    TV = false; walk(10); assert(today() == 10);
    /* tout coupé */
    S.step_count = S.wrist_wake = false; motion_apply(); assert(!ACC && !motion_steps_on() && !motion_on_irq());
    /* confirmation du geste : écran vers le haut, immobile */
    S.wrist_wake = true; motion_apply();
    assert(motion_wrist_confirm(0) && motion_wrist_confirm(1) && motion_wrist_confirm(2));
    S.wrist_wake = false; motion_apply();
    printf("OK (%d écritures NVS)\n", N_writes);
    return 0;
}
#include "i2c_bus.h"
#include "app_events.h"
esp_err_t i2c_bus_add(i2c_bus_id_t b, uint8_t a, i2c_master_dev_handle_t *o) { *o = (void *)1; return 0; }
esp_err_t i2c_reg_write8(i2c_master_dev_handle_t d, uint8_t r, uint8_t v) { if (r == 0x7D) ACC = v; return 0; }
esp_err_t i2c_reg_read8(i2c_master_dev_handle_t d, uint8_t r, uint8_t *v) { *v = 0; return 0; }
esp_err_t i2c_reg_read(i2c_master_dev_handle_t d, uint8_t r, uint8_t *b, size_t n) { memset(b, 0, n); return 0; }   /* accéléromètre brut */
void ble_diag(const char *f, ...) {}
bool app_post_from_isr(const app_event_t *e, BaseType_t *h) { return true; }
