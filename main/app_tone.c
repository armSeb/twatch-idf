/*
 * app_tone.c — « Tone » : tonalité d'ouverture de relais (1750 Hz par défaut)
 * sur le haut-parleur, à approcher du micro du poste.
 *
 *   ‹ Tone
 *         1750            fréquence en Hz : toucher un chiffre = pas de − / +
 *          Hz
 *   [ 1750 | 2135 | 1000 ]  préréglages
 *    [ − ]  [  ((•))  ]  [ + ]   maintenir le bouton central pour émettre
 *
 * Un appui bref émet quand même TONE_MIN_MS (salve). Sinusoïde à 16 kHz avec
 * montée et descente de 5 ms (pas de clic), générée par une petite tâche qui
 * alimente le flux I2S de sound.c.
 */
#include <math.h>
#include <stdio.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "sound.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define RATE        16000
#define CHUNK       160                 /* 10 ms */
#define FADE        (RATE * 5 / 1000)   /* 5 ms */
#define TONE_MIN_MS 1000                /* durée minimale d'une salve */
#define F_MIN       100
#define F_MAX       4000
#define F_DEF       1750
#define NDIG        4
#define AMP         26000.0f            /* ≈ -2 dBFS : fort, sans saturer l'ampli */

static const uint16_t PRESET[] = { 1750, 2135, 1000 };
static const int W[NDIG] = { 1000, 100, 10, 1 };

static int s_f = F_DEF;
static int s_dig = 1;                   /* pas : 100 Hz */
static volatile bool s_hold;            /* bouton enfoncé */
static volatile int64_t s_until;        /* fin minimale de la salve (µs) */
static volatile bool s_task;            /* tâche d'émission en cours */
static bool s_loaded;

static lv_obj_t *v_dig[NDIG], *v_seg, *v_btn, *v_btn_lbl, *v_hint;

/* ------------------------------------------------------------------ */
static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open("tone", NVS_READONLY, &h) != ESP_OK) return;
    uint16_t f;
    if (nvs_get_u16(h, "f", &f) == ESP_OK && f >= F_MIN && f <= F_MAX) s_f = f;
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("tone", NVS_READWRITE, &h) != ESP_OK) return;
    uint16_t old = 0;
    if (nvs_get_u16(h, "f", &old) != ESP_OK || old != s_f) {
        nvs_set_u16(h, "f", (uint16_t)s_f);
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
static void tone_task(void *arg)
{
    int16_t buf[CHUNK];
    do {                                  /* rappuyé pendant la fermeture : on repart */
        if (sound_stream_open(RATE) != ESP_OK) break;
        float ph = 0, env = 0;
        const float dEnv = 1.0f / FADE;
        for (;;) {
            bool on = s_hold || esp_timer_get_time() < s_until;
            float dph = 2.0f * (float)M_PI * s_f / RATE;     /* fréquence modifiable en direct */
            for (int i = 0; i < CHUNK; i++) {
                env += on ? dEnv : -dEnv;
                if (env > 1) env = 1;
                if (env < 0) env = 0;
                buf[i] = (int16_t)(AMP * env * sinf(ph));
                ph += dph;
                if (ph > 2 * (float)M_PI) ph -= 2 * (float)M_PI;
            }
            sound_stream_write(buf, CHUNK, 4096);
            if (!on && env <= 0) break;
        }
        sound_stream_write(NULL, CHUNK * 4, 0);          /* silence : vide le DMA */
        sound_stream_close();
    } while (s_hold);
    s_task = false;
    vTaskDelete(NULL);
}

static void tone_on(void)
{
    s_hold = true;
    s_until = esp_timer_get_time() + TONE_MIN_MS * 1000LL;
    if (!s_task) {
        s_task = true;
        if (xTaskCreatePinnedToCore(tone_task, "tone", 3072, NULL, 6, NULL, 1) != pdPASS) s_task = false;
    }
}

static void tone_off(void) { s_hold = false; }

/* ------------------------------------------------------------------ */
static void refresh(void)
{
    if (!v_dig[0]) return;
    int f = s_f;
    bool lead = true;
    for (int i = 0; i < NDIG; i++) {
        int d = (f / W[i]) % 10;
        char c[2] = { (char)('0' + d), 0 };
        lv_label_set_text(v_dig[i], c);
        bool zero = lead && d == 0 && i < NDIG - 1;
        if (!zero) lead = false;
        lv_color_t col = i == s_dig ? C_ACC : zero ? lv_color_hex(0x3A3F47) : C_FG;
        lv_obj_set_style_text_color(v_dig[i], col, 0);
        lv_obj_set_style_border_width(v_dig[i], i == s_dig ? 3 : 0, 0);
    }
    for (int i = 0; i < (int)(sizeof PRESET / sizeof PRESET[0]); i++) {
        if (PRESET[i] == s_f) lv_buttonmatrix_set_button_ctrl(v_seg, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        else lv_buttonmatrix_clear_button_ctrl(v_seg, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

static void btn_look(bool on)
{
    lv_obj_set_style_bg_color(v_btn, on ? lv_color_hex(0xDC2626) : lv_color_hex(0xEC4899), 0);
}

static void change(int dir)
{
    int f = s_f + dir * W[s_dig];
    if (f < F_MIN) f = F_MIN;
    if (f > F_MAX) f = F_MAX;
    s_f = f;
    refresh();
}

static void minus_cb(lv_event_t *e) { change(-1); }
static void plus_cb(lv_event_t *e)  { change(+1); }

static void dig_cb(lv_event_t *e)
{
    s_dig = (int)(intptr_t)lv_event_get_user_data(e);
    refresh();
}

static void seg_cb(lv_event_t *e)
{
    int p = (int)lv_buttonmatrix_get_selected_button(v_seg);
    if (p < 0 || p >= (int)(sizeof PRESET / sizeof PRESET[0])) return;
    s_f = PRESET[p];
    refresh();
}

/* bouton d'émission : appuyé = tonalité, relâché = fin (après la salve
 * minimale) ; on ne le laisse pas partir en geste de retour */
static void tx_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) { tone_on(); btn_look(true); }
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) { tone_off(); btn_look(false); }
}

static lv_obj_t *round_btn(lv_obj_t *scr, const char *txt, lv_event_cb_t cb, lv_color_t bg, int x, int w)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, w, 56);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -8);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    }
    ui_bubble(b);
    lv_obj_t *l = ui_label(b, txt[0] == '+' || txt[0] == '-' ? &font_num_44 : &font_title_20, C_FG, txt);
    lv_obj_center(l);
    return b;
}

static void create(lv_obj_t *scr)
{
    load();
    int y = ui_header(scr, "Tone");

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 236, 56);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    ui_bubble(row);
    for (int i = 0; i < NDIG; i++) {
        v_dig[i] = ui_label(row, &font_num_44, C_FG, "0");
        lv_obj_set_style_border_side(v_dig[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(v_dig[i], C_ACC, 0);
        lv_obj_add_flag(v_dig[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(v_dig[i], 6);
        lv_obj_add_event_cb(v_dig[i], dig_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_bubble(v_dig[i]);
    }
    lv_obj_t *hz = ui_label(row, &font_title_20, C_DIM, "Hz");
    lv_obj_set_style_pad_left(hz, 6, 0);
    lv_obj_set_style_pad_top(hz, 14, 0);

    static const char *map[] = { "1750", "2135", "1000", "" };
    v_seg = ui_segmented(scr, map, seg_cb);
    lv_obj_set_size(v_seg, 216, 32);
    lv_obj_align(v_seg, LV_ALIGN_TOP_MID, 0, y + 62);

    v_hint = ui_label(scr, &font_small_11, C_DIM, _("Maintenir pour émettre · 1 s au moins"));
    lv_obj_align(v_hint, LV_ALIGN_TOP_MID, 0, y + 100);

    round_btn(scr, "-", minus_cb, C_OFF, -84, 56);
    v_btn = round_btn(scr, LV_SYMBOL_BELL, NULL, lv_color_hex(0xEC4899), 0, 96);
    v_btn_lbl = lv_obj_get_child(v_btn, 0);
    lv_obj_add_event_cb(v_btn, tx_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(v_btn, tx_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(v_btn, tx_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_remove_flag(v_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    round_btn(scr, "+", plus_cb, C_OFF, 84, 56);
    refresh();
}

static void destroy(void)
{
    tone_off();
    s_until = 0;
    save();
    for (int i = 0; i < NDIG; i++) v_dig[i] = NULL;
    v_seg = v_btn = v_btn_lbl = v_hint = NULL;
}

static void on_show(void) { refresh(); }

static void on_hide(void)
{
    tone_off();                         /* écran éteint ou sortie : plus de son */
    s_until = 0;
    if (v_btn) btn_look(false);
    save();
}

static bool busy(void) { return s_task; }

const app_t app_tone = {
    .name = "Tone",
    .icon = LV_SYMBOL_BELL,
    .color = 0xEC4899,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .busy = busy,
};
