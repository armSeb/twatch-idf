#include "display.h"
#include "board.h"
#include "power.h"
#include "i2c_bus.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static i2c_master_dev_handle_t s_touch;
static bool s_on;
static bool s_turbo;     /* verrou CPU_FREQ_MAX tenu */

/* Écran allumé sans interaction ni animation depuis TURBO_IDLE_MS : le CPU
 * redescend à POWER_CPU_MIN_MHZ (l'aiguille des secondes se dessine très bien
 * à 40 MHz). Il remonte dès qu'un toucher remet l'inactivité LVGL à zéro. */
#define TURBO_IDLE_MS 1500

#define BUF_LINES 60   /* 2 × 28,8 Ko DMA : moins de découpage pendant les animations */

/* ------------------------------------------------------------------ */
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    lv_draw_sw_rgb565_swap(px, lv_area_get_size(a));
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

/* ------------------------------------------------------------------ */
/* FT6336 : lecture directe des registres 0x02..0x06                   */
/* ------------------------------------------------------------------ */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int16_t last_x, last_y;
    uint8_t b[5];
    data->state = LV_INDEV_STATE_RELEASED;
    if (i2c_reg_read(s_touch, 0x02, b, 5) == ESP_OK) {
        uint8_t n = b[0] & 0x0F;
        if (n >= 1 && n <= 2) {
            int16_t x = ((b[1] & 0x0F) << 8) | b[2];
            int16_t y = ((b[3] & 0x0F) << 8) | b[4];
#if BOARD_TOUCH_INVERT
            x = BOARD_LCD_W - 1 - x;
            y = BOARD_LCD_H - 1 - y;
#endif
            if (x >= 0 && x < BOARD_LCD_W && y >= 0 && y < BOARD_LCD_H) {
                last_x = x;
                last_y = y;
                data->state = LV_INDEV_STATE_PRESSED;
            }
        }
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

static void touch_power(bool active)
{
    /* 0xA5 : 0 = actif, 1 = monitor (scan lent, retour auto en actif) */
    i2c_reg_write8(s_touch, 0xA5, active ? 0x00 : 0x01);
}

/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_W * BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx = s_disp,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &pcfg, &s_panel));
    esp_lcd_panel_reset(s_panel);          /* reset logiciel (pas de broche RST) */
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);

    size_t bsz = BOARD_LCD_W * BUF_LINES * 2;
    void *b1 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_buffers(s_disp, b1, b2, bsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    /* tactile */
    if (i2c_bus_add(I2C_BUS_TOUCH, BOARD_FT6336_ADDR, &s_touch) == ESP_OK) {
        uint8_t vid = 0;
        i2c_reg_read8(s_touch, 0xA8, &vid);
        ESP_LOGI(TAG, "FT6336 vendor 0x%02x", vid);
    }
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    /* l'écran reste éteint (veille) jusqu'au premier display_set_on(true) */
    esp_lcd_panel_disp_on_off(s_panel, false);
    s_on = false;
    touch_power(false);
    lv_indev_enable(s_indev, false);
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

void display_set_on(bool on)
{
    if (on == s_on) return;
    if (on) {
        power_hold_nosleep(true);                /* LEDC sur REF_TICK : pas de light sleep écran allumé */
        power_hold_active(true);                 /* CPU max pour le premier rendu */
        s_turbo = true;
        esp_lcd_panel_disp_sleep(s_panel, false);/* SLPOUT (+ délai driver) */
        touch_power(true);
        lv_indev_reset(s_indev, NULL);
        lv_indev_enable(s_indev, true);
        lv_display_trigger_activity(s_disp);
        /* on peint une image fraîche AVANT d'allumer le rétroéclairage */
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_disp);
        esp_lcd_panel_disp_on_off(s_panel, true);
        power_backlight(settings_get()->brightness);
        s_on = true;
    } else {
        power_backlight(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_disp_sleep(s_panel, true); /* SLPIN : ~10 µA */
        lv_indev_enable(s_indev, false);
        touch_power(false);
        s_on = false;
        if (s_turbo) { power_hold_active(false); s_turbo = false; }
        power_hold_nosleep(false);
    }
}

bool display_is_on(void) { return s_on; }

void display_perf_tick(void)
{
    if (!s_on) return;
    bool busy = lv_display_get_inactive_time(s_disp) < TURBO_IDLE_MS
                || lv_anim_count_running() > 0;
    if (busy != s_turbo) {
        power_hold_active(busy);
        s_turbo = busy;
    }
}

void display_set_brightness(uint8_t level)
{
    if (s_on) power_backlight(level);
}
