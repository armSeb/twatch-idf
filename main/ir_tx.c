/* ir_tx.c — voir ir_tx.h */
#include "ir_tx.h"
#include "board.h"

static bool s_toggle;

#ifdef ESP_PLATFORM
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;
static int s_carrier;

esp_err_t ir_tx_open(void)
{
    if (s_ch) return ESP_OK;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = BOARD_IR_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,                      /* 1 µs */
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    esp_err_t e = rmt_new_tx_channel(&cfg, &s_ch);
    if (e != ESP_OK) { s_ch = NULL; return e; }
    rmt_copy_encoder_config_t ec = {};
    if ((e = rmt_new_copy_encoder(&ec, &s_enc)) != ESP_OK) { ir_tx_close(); return e; }
    s_carrier = 0;
    return ESP_OK;
}

esp_err_t ir_tx_send(const ir_code_t *c, bool held)
{
    if (!s_ch) return ESP_ERR_INVALID_STATE;
    if (!held && (c->proto == IR_RC5 || c->proto == IR_RC6)) s_toggle = !s_toggle;
    static uint16_t us[200];
    static rmt_symbol_word_t sym[110];
    int n = ir_encode(c, s_toggle, us, 200);
    if (n <= 0) return ESP_ERR_INVALID_ARG;
    /* paires marque / espace ; une durée > 32767 µs est coupée en deux */
    int k = 0;
    for (int i = 0; i < n && k < 110; i += 2) {
        int m = us[i], s = i + 1 < n ? us[i + 1] : 100;
        int s1 = s > 32000 ? 32000 : s;
        sym[k++] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = (uint16_t)m, .level1 = 0, .duration1 = (uint16_t)s1 };
        if (s > 32000 && k < 110) sym[k++] = (rmt_symbol_word_t){ .level0 = 0, .duration0 = (uint16_t)(s - s1), .level1 = 0, .duration1 = 1 };
    }
    int hz = ir_proto_carrier(c->proto);
    esp_err_t e = ESP_OK;
    if (hz != s_carrier) {                             /* porteuse : canal arrêté pour la changer */
        if (s_carrier) rmt_disable(s_ch);
        rmt_carrier_config_t car = { .frequency_hz = (uint32_t)hz, .duty_cycle = 0.33f };
        if ((e = rmt_apply_carrier(s_ch, &car)) != ESP_OK || (e = rmt_enable(s_ch)) != ESP_OK) { s_carrier = 0; return e; }
        s_carrier = hz;
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    e = rmt_transmit(s_ch, s_enc, sym, (size_t)k * sizeof sym[0], &tc);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(s_ch, 400);
    return e;
}

void ir_tx_close(void)
{
    if (s_ch) {
        if (s_carrier) rmt_disable(s_ch);
        rmt_del_channel(s_ch);
        s_ch = NULL;
    }
    if (s_enc) { rmt_del_encoder(s_enc); s_enc = NULL; }
    s_carrier = 0;
}
#else
/* simulateur : on garde la trace des envois */
ir_code_t g_ir_last;
int g_ir_sent;
esp_err_t ir_tx_open(void) { return ESP_OK; }
esp_err_t ir_tx_send(const ir_code_t *c, bool held)
{
    if (!held && (c->proto == IR_RC5 || c->proto == IR_RC6)) s_toggle = !s_toggle;
    g_ir_last = *c;
    g_ir_sent++;
    return ESP_OK;
}
void ir_tx_close(void) {}
#endif
