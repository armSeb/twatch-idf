/*
 * app_rfgen.c — mini générateur HF : la PLL audio (APLL) de l'ESP32 sortie
 * directement sur une broche CLK_OUT (signal carré, 5,3 à 125 MHz).
 *
 * Broches possibles sur l'ESP32 : GPIO0, 1 et 3 seulement (fonction CLK_OUT
 * de l'IO_MUX, la matrice GPIO plafonnerait vers 40 MHz). Sur la T-Watch
 * 2020 V1, GPIO0 ne va qu'à l'auto-reset USB (UMH3N) et GPIO1 (U0TXD) au
 * CP2104 via 470 Ω : pas d'antenne, portée de quelques centimètres à
 * quelques mètres selon la fréquence. Carré = harmoniques impaires fortes.
 *
 * Précision : celle du quartz 40 MHz (±10 ppm). La fréquence réellement
 * programmée (pas de quelques Hz) est affichée sous la consigne.
 */
#include <stdio.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "power.h"
#include "esp_timer.h"
#include "esp_clock_output.h"
#include "clk_ctrl_os.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/io_mux_reg.h"

#define F_MIN      5310000u          /* CLK_LL_APLL_MIN_HZ arrondi */
#define F_MAX    125000000u
#define NDIG       7                  /* 100 MHz … 100 Hz : « 100.0000 » MHz */
#define AUTO_OFF_MIN 10
#define EV_AUTO_OFF 1

extern const app_t app_rfgen;

/* modèle */
static uint32_t s_f = 100000000u;     /* consigne (Hz) */
static uint32_t s_real;               /* fréquence programmée (Hz) */
static int  s_dig = 3;                /* chiffre sélectionné (0 = centaines de MHz) */
static int  s_pin = 0;                /* 0 = GPIO0, 1 = GPIO1 */
static bool s_run, s_err;
static esp_clock_output_mapping_handle_t s_map;
static esp_timer_handle_t s_off_t;
static const uint32_t W[NDIG] = { 100000000, 10000000, 1000000, 100000, 10000, 1000, 100 };

/* vue */
static lv_obj_t *v_dig[NDIG], *v_real, *v_btn_lbl, *v_btn, *v_seg;

static gpio_num_t pin_gpio(void) { return s_pin ? GPIO_NUM_1 : GPIO_NUM_0; }

static void off_cb(void *arg) { app_post_event(&app_rfgen, EV_AUTO_OFF); }

static void restore_pin(gpio_num_t g)
{
    if (g == GPIO_NUM_1) gpio_iomux_output(GPIO_NUM_1, FUNC_U0TXD_U0TXD);   /* console UART */
    else gpio_reset_pin(g);                                                 /* entrée + pull-up (strap) */
}

static void gen_stop(void)
{
    if (!s_run) return;
    if (s_off_t) esp_timer_stop(s_off_t);
    if (s_map) { esp_clock_output_stop(s_map); s_map = NULL; }
    periph_rtc_apll_release();
    restore_pin(pin_gpio());
    power_hold_nosleep(false);        /* le light sleep couperait l'APLL */
    s_run = false;
}

static bool apll_set(void)
{
    uint32_t real = 0;
    if (periph_rtc_apll_freq_set(s_f, &real) != ESP_OK) return false;
    s_real = real;
    return true;
}

static void gen_start(void)
{
    s_err = false;
    periph_rtc_apll_acquire();
    if (!apll_set() || esp_clock_output_start(CLKOUT_SIG_APLL, pin_gpio(), &s_map) != ESP_OK) {
        s_map = NULL;
        periph_rtc_apll_release();
        s_err = true;
        return;
    }
    gpio_set_drive_capability(pin_gpio(), GPIO_DRIVE_CAP_3);   /* ~40 mA, le maximum */
    power_hold_nosleep(true);
    s_run = true;
    if (!s_off_t) {
        const esp_timer_create_args_t a = { .callback = off_cb, .name = "rfgen" };
        esp_timer_create(&a, &s_off_t);
    }
    if (s_off_t) { esp_timer_stop(s_off_t); esp_timer_start_once(s_off_t, AUTO_OFF_MIN * 60ULL * 1000000ULL); }
}

static void refresh(void)
{
    if (!v_dig[0]) return;
    uint32_t f = s_f / 100;           /* en centaines de Hz : 7 chiffres */
    bool lead = true;
    for (int i = NDIG - 1; i >= 0; i--) {
        char c[2] = { (char)('0' + f % 10), 0 };
        f /= 10;
        lv_label_set_text(v_dig[i], c);
    }
    for (int i = 0; i < NDIG; i++) {
        bool zero = lead && i < 2 && lv_label_get_text(v_dig[i])[0] == '0';
        if (!zero) lead = false;
        lv_color_t col = i == s_dig ? C_ACC : zero ? lv_color_hex(0x3A3F47) : C_FG;
        lv_obj_set_style_text_color(v_dig[i], col, 0);
        lv_obj_set_style_border_width(v_dig[i], i == s_dig ? 3 : 0, 0);
    }
    char b[48];
    if (s_err) snprintf(b, sizeof b, "%s", _("Erreur APLL"));
    else if (s_run) {
        char n[16];
        unsigned long r = s_real;
        snprintf(n, sizeof n, "%lu %03lu %03lu", r / 1000000, (r / 1000) % 1000, r % 1000);
        snprintf(b, sizeof b, _("MHz · réel %s Hz"), n);
    } else snprintf(b, sizeof b, "%s", _("MHz · arrêté"));
    lv_label_set_text(v_real, b);
    lv_label_set_text(v_btn_lbl, s_run ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(v_btn, s_run ? lv_color_hex(0xDC2626) : lv_color_hex(0x16A34A), 0);
}

static void change(int dir)
{
    int64_t f = (int64_t)s_f + dir * (int64_t)W[s_dig];
    if (f < F_MIN) f = F_MIN;
    if (f > F_MAX) f = F_MAX;
    s_f = (uint32_t)f;
    if (s_run && !apll_set()) s_err = true;
    refresh();
}

static void minus_cb(lv_event_t *e) { change(-1); }
static void plus_cb(lv_event_t *e)  { change(+1); }

static void dig_cb(lv_event_t *e)
{
    s_dig = (int)(intptr_t)lv_event_get_user_data(e);
    refresh();
}

static void run_cb(lv_event_t *e)
{
    if (s_run) gen_stop(); else gen_start();
    refresh();
}

static void seg_cb(lv_event_t *e)
{
    int p = (int)lv_buttonmatrix_get_selected_button(v_seg);
    if (p < 0 || p > 1 || p == s_pin) return;
    bool was = s_run;
    gen_stop();
    s_pin = p;
    if (was) gen_start();
    refresh();
}

static lv_obj_t *round_btn(lv_obj_t *scr, const char *txt, lv_event_cb_t cb, lv_color_t bg, int x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 64, 56);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -8);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    ui_bubble(b);
    /* « + » et « – » en grand (police des chiffres), symboles en 20 px */
    lv_obj_t *l = ui_label(b, txt[0] == '+' || txt[0] == '-' ? &font_num_44 : &font_title_20, C_FG, txt);
    lv_obj_center(l);
    return b;
}

static void create(lv_obj_t *scr)
{
    int y = ui_header(scr, _("Générateur HF"));

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 236, 56);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y + 2);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_bubble(row);
    for (int i = 0; i < NDIG; i++) {
        if (i == 3) ui_label(row, &font_num_44, C_DIM, ".");
        v_dig[i] = ui_label(row, &font_num_44, C_FG, "0");
        lv_obj_set_style_border_side(v_dig[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(v_dig[i], C_ACC, 0);
        lv_obj_add_flag(v_dig[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(v_dig[i], 6);
        lv_obj_add_event_cb(v_dig[i], dig_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_bubble(v_dig[i]);
    }

    v_real = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_align(v_real, LV_ALIGN_TOP_MID, 0, y + 62);

    static const char *map[] = { "GPIO0", "GPIO1", "" };
    v_seg = ui_segmented(scr, map, seg_cb);
    lv_obj_set_size(v_seg, 200, 32);
    lv_obj_align(v_seg, LV_ALIGN_TOP_MID, 0, y + 82);
    lv_buttonmatrix_set_button_ctrl(v_seg, s_pin, LV_BUTTONMATRIX_CTRL_CHECKED);

    round_btn(scr, "-", minus_cb, C_OFF, -76);
    v_btn = round_btn(scr, LV_SYMBOL_PLAY, run_cb, lv_color_hex(0x16A34A), 0);
    v_btn_lbl = lv_obj_get_child(v_btn, 0);
    lv_obj_remove_event_cb(v_btn, run_cb);                    /* pas de répétition sur marche/arrêt */
    lv_obj_add_event_cb(v_btn, run_cb, LV_EVENT_CLICKED, NULL);
    round_btn(scr, "+", plus_cb, C_OFF, 76);
    refresh();
}

static void destroy(void)
{
    for (int i = 0; i < NDIG; i++) v_dig[i] = NULL;
    v_real = v_btn = v_btn_lbl = v_seg = NULL;
}

static void on_show(void) { refresh(); }

static bool on_event(int code)
{
    if (code == EV_AUTO_OFF && s_run) {
        gen_stop();
        power_vibrate(VIB_SHORT);
        refresh();
    }
    return false;
}

static bool busy(void) { return s_run; }

const app_t app_rfgen = {
    .name = N_("Porteuse"),
    .icon = SYM_TOWER,
    .color = 0xF59E0B,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_event = on_event,
    .busy = busy,
};
