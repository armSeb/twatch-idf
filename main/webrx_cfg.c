/* webrx_cfg.c — voir webrx_cfg.h */
#include "webrx_cfg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs.h"
#include "esp_heap_caps.h"

#define NS "webrx"

static void get_str(nvs_handle_t h, const char *k, char *out, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, k, out, &l) != ESP_OK) out[0] = 0;
}

void wrx_cfg_load(wrx_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < WRX_NETS; i++) {
        char k[4] = { 's', (char)('0' + i), 0 };
        get_str(h, k, c->ssid[i], sizeof c->ssid[i]);
        k[0] = 'p';
        get_str(h, k, c->pass[i], sizeof c->pass[i]);
    }
    get_str(h, "url", c->url, sizeof c->url);
    nvs_close(h);
}

esp_err_t wrx_cfg_save(const wrx_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (int i = 0; i < WRX_NETS && e == ESP_OK; i++) {
        char k[4] = { 's', (char)('0' + i), 0 };
        e = nvs_set_str(h, k, c->ssid[i]);
        k[0] = 'p';
        if (e == ESP_OK) e = nvs_set_str(h, k, c->pass[i]);
    }
    if (e == ESP_OK) e = nvs_set_str(h, "url", c->url);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool wrx_cfg_ready(void)
{
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return false;
    wrx_cfg_load(c);
    bool net = false;
    for (int i = 0; i < WRX_NETS; i++) net |= c->ssid[i][0] != 0;
    bool ok = net && c->url[0];
    free(c);
    return ok;
}

void wrx_state_load(wrx_state_t *s)
{
    memset(s, 0, sizeof *s);
    s->volume = 60;
    s->squelch = -150;
    s->filter = 1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "prof", s->profile, sizeof s->profile);
    get_str(h, "mod", s->mod, sizeof s->mod);
    int64_t f;
    if (nvs_get_i64(h, "freq", &f) == ESP_OK) s->freq = f;
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s->volume = v;
    int16_t q;
    if (nvs_get_i16(h, "sq", &q) == ESP_OK) s->squelch = q;
    uint8_t fl;
    if (nvs_get_u8(h, "flt", &fl) == ESP_OK) s->filter = fl ? 1 : 0;
    nvs_close(h);
}

void wrx_state_save(const wrx_state_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    /* n'écrire que ce qui change : la flash n'aime pas les écritures inutiles */
    wrx_state_t old;
    wrx_state_load(&old);
    if (strcmp(old.profile, s->profile)) nvs_set_str(h, "prof", s->profile);
    if (strcmp(old.mod, s->mod)) nvs_set_str(h, "mod", s->mod);
    if (old.freq != s->freq) nvs_set_i64(h, "freq", s->freq);
    if (old.volume != s->volume) nvs_set_u8(h, "vol", s->volume);
    if (old.squelch != s->squelch) nvs_set_i16(h, "sq", s->squelch);
    if (old.filter != s->filter) nvs_set_u8(h, "flt", s->filter);
    nvs_commit(h);
    nvs_close(h);
}
