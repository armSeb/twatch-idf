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
    for (int i = 0; i < WRX_SRV; i++) {
        char k[4] = { 'u', (char)('0' + i), 0 };
        get_str(h, k, c->srv[i].url, sizeof c->srv[i].url);
        k[0] = 'n';
        get_str(h, k, c->srv[i].name, sizeof c->srv[i].name);
    }
    if (!c->srv[0].url[0]) get_str(h, "url", c->srv[0].url, sizeof c->srv[0].url);   /* ≤ 0.22 */
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
    for (int i = 0; i < WRX_SRV && e == ESP_OK; i++) {
        char k[4] = { 'u', (char)('0' + i), 0 };
        e = nvs_set_str(h, k, c->srv[i].url);
        k[0] = 'n';
        if (e == ESP_OK) e = nvs_set_str(h, k, c->srv[i].name);
    }
    nvs_erase_key(h, "url");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int wrx_cfg_servers(const wrx_cfg_t *c)
{
    int n = 0;
    while (n < WRX_SRV && c->srv[n].url[0]) n++;
    return n;
}

void wrx_srv_label(const wrx_cfg_t *c, int i, char *out, size_t cap)
{
    if (c->srv[i].name[0]) { strlcpy(out, c->srv[i].name, cap); return; }
    const char *u = c->srv[i].url;
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    const char *at = strchr(p, '@'), *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) p = at + 1;              /* identifiants masqués */
    size_t l = strcspn(p, ":/?#");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

bool wrx_cfg_ready(void)
{
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return false;
    wrx_cfg_load(c);
    bool net = false;
    for (int i = 0; i < WRX_NETS; i++) net |= c->ssid[i][0] != 0;
    bool ok = net && c->srv[0].url[0];
    free(c);
    return ok;
}

/* clés par serveur : « prof0 », « freq2 »… ; le serveur 0 garde aussi les
 * anciennes clés sans numéro (≤ 0.22) */
static void key(char *k, const char *base, int srv) { snprintf(k, 12, "%s%d", base, srv); }

void wrx_state_load_srv(wrx_state_t *s, int server)
{
    memset(s, 0, sizeof *s);
    s->squelch = -150;
    s->tone = WRX_TONE_DEF;
    s->volume = VOL_DEF;
    s->server = (uint8_t)(server >= 0 && server < WRX_SRV ? server : 0);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "tone", &v) == ESP_OK && v < WRX_TONES) s->tone = v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v >= 1 && v <= 100) s->volume = v;
    char k[12];
    int i = s->server;
    key(k, "prof", i);
    get_str(h, k, s->profile, sizeof s->profile);
    if (!s->profile[0] && i == 0) get_str(h, "prof", s->profile, sizeof s->profile);
    key(k, "mod", i);
    get_str(h, k, s->mod, sizeof s->mod);
    if (!s->mod[0] && i == 0) get_str(h, "mod", s->mod, sizeof s->mod);
    int64_t f;
    key(k, "freq", i);
    if (nvs_get_i64(h, k, &f) == ESP_OK || (i == 0 && nvs_get_i64(h, "freq", &f) == ESP_OK)) s->freq = f;
    int16_t q;
    key(k, "sq", i);
    if (nvs_get_i16(h, k, &q) == ESP_OK || (i == 0 && nvs_get_i16(h, "sq", &q) == ESP_OK)) s->squelch = q;
    nvs_close(h);
}

void wrx_state_load(wrx_state_t *s)
{
    uint8_t srv = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "srv", &srv);
        nvs_close(h);
    }
    wrx_state_load_srv(s, srv);
}

void wrx_state_save(const wrx_state_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    /* n'écrire que ce qui change : la flash n'aime pas les écritures inutiles */
    wrx_state_t old;
    wrx_state_load_srv(&old, s->server);
    uint8_t cur = 0xFF;
    nvs_get_u8(h, "srv", &cur);
    if (cur != s->server) nvs_set_u8(h, "srv", s->server);
    if (old.tone != s->tone) nvs_set_u8(h, "tone", s->tone);
    if (s->volume >= 1 && s->volume <= 100 && old.volume != s->volume) nvs_set_u8(h, "vol", s->volume);
    char k[12];
    int i = s->server;
    key(k, "prof", i);
    if (strcmp(old.profile, s->profile)) nvs_set_str(h, k, s->profile);
    key(k, "mod", i);
    if (strcmp(old.mod, s->mod)) nvs_set_str(h, k, s->mod);
    key(k, "freq", i);
    if (old.freq != s->freq) nvs_set_i64(h, k, s->freq);
    key(k, "sq", i);
    if (old.squelch != s->squelch) nvs_set_i16(h, k, s->squelch);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- webradios ---------------- */
#define RNS "radio"

int rad_cfg_load(rad_station_t st[RAD_MAX])
{
    memset(st, 0, RAD_MAX * sizeof *st);
    nvs_handle_t h;
    if (nvs_open(RNS, NVS_READONLY, &h) != ESP_OK) return 0;
    int n = 0;
    for (int i = 0; i < RAD_MAX; i++) {
        char k[8];
        snprintf(k, sizeof k, "u%d", i);
        get_str(h, k, st[n].url, sizeof st[n].url);
        snprintf(k, sizeof k, "n%d", i);
        get_str(h, k, st[n].name, sizeof st[n].name);
        if (st[n].url[0]) n++;
        else st[n].name[0] = 0;
    }
    nvs_close(h);
    return n;
}

esp_err_t rad_cfg_save(const rad_station_t st[RAD_MAX])
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RNS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (int i = 0; i < RAD_MAX && e == ESP_OK; i++) {
        char k[8];
        snprintf(k, sizeof k, "u%d", i);
        e = nvs_set_str(h, k, st[i].url);
        snprintf(k, sizeof k, "n%d", i);
        if (e == ESP_OK) e = nvs_set_str(h, k, st[i].name);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void rad_state_load(rad_state_t *s)
{
    s->station = 0;
    s->tone = 1;                                      /* musique : « claire » */
    s->volume = VOL_DEF;
    nvs_handle_t h;
    if (nvs_open(RNS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "st", &v) == ESP_OK && v < RAD_MAX) s->station = v;
    if (nvs_get_u8(h, "tone", &v) == ESP_OK && v < WRX_TONES) s->tone = v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v >= 1 && v <= 100) s->volume = v;
    nvs_close(h);
}

void rad_state_save(const rad_state_t *s)
{
    rad_state_t old;
    rad_state_load(&old);
    bool vol = s->volume >= 1 && s->volume <= 100 && s->volume != old.volume;
    if (old.station == s->station && old.tone == s->tone && !vol) return;
    nvs_handle_t h;
    if (nvs_open(RNS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "st", s->station);
    nvs_set_u8(h, "tone", s->tone);
    if (vol) nvs_set_u8(h, "vol", s->volume);
    nvs_commit(h);
    nvs_close(h);
}

void rad_label(const rad_station_t *st, char *out, size_t cap)
{
    if (st->name[0]) { strlcpy(out, st->name, cap); return; }
    const char *u = st->url;
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    const char *at = strchr(p, '@'), *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) p = at + 1;
    size_t l = strcspn(p, ":/?#");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
}
