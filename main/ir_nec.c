/* ir_nec.c — voir ir_nec.h */
#include "ir_nec.h"
#include "board.h"
#ifdef ESP_PLATFORM
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;

#define US(x) (x)                                      /* résolution : 1 µs */

esp_err_t ir_nec_open(void)
{
    if (s_ch) return ESP_OK;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = BOARD_IR_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    esp_err_t e = rmt_new_tx_channel(&cfg, &s_ch);
    if (e != ESP_OK) { s_ch = NULL; return e; }
    rmt_carrier_config_t car = { .frequency_hz = 38000, .duty_cycle = 0.33f };
    rmt_copy_encoder_config_t ec = {};
    if ((e = rmt_apply_carrier(s_ch, &car)) != ESP_OK || (e = rmt_new_copy_encoder(&ec, &s_enc)) != ESP_OK
        || (e = rmt_enable(s_ch)) != ESP_OK) {
        ir_nec_close();
        return e;
    }
    return ESP_OK;
}

esp_err_t ir_nec_send(uint16_t addr, uint8_t cmd)
{
    if (!s_ch) return ESP_ERR_INVALID_STATE;
    static rmt_symbol_word_t sym[34];
    uint32_t bits = (uint32_t)addr | ((uint32_t)cmd << 16) | ((uint32_t)(uint8_t)~cmd << 24);
    sym[0] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = US(9000), .level1 = 0, .duration1 = US(4500) };
    for (int i = 0; i < 32; i++)
        sym[1 + i] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = US(562), .level1 = 0,
                                          .duration1 = (bits >> i) & 1 ? US(1687) : US(562) };
    sym[33] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = US(562), .level1 = 0, .duration1 = US(100) };
    rmt_transmit_config_t tc = { .loop_count = 0 };
    esp_err_t e = rmt_transmit(s_ch, s_enc, sym, sizeof sym, &tc);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(s_ch, 200);
    return e;
}

void ir_nec_close(void)
{
    if (s_ch) { rmt_disable(s_ch); rmt_del_channel(s_ch); s_ch = NULL; }
    if (s_enc) { rmt_del_encoder(s_enc); s_enc = NULL; }
}
#else
/* simulateur : on garde la trace des envois */
uint16_t g_ir_last_addr; uint8_t g_ir_last_cmd; int g_ir_sent;
esp_err_t ir_nec_open(void) { return ESP_OK; }
esp_err_t ir_nec_send(uint16_t addr, uint8_t cmd) { g_ir_last_addr = addr; g_ir_last_cmd = cmd; g_ir_sent++; return ESP_OK; }
void ir_nec_close(void) {}
#endif
