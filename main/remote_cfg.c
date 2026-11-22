/* remote_cfg.c — voir remote_cfg.h */
#include "remote_cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cJSON.h"
#include "i18n.h"
#ifndef RC_NO_NVS
#include "esp_err.h"
#include "nvs.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* les jeux de télécommandes font ~6 Ko : en PSRAM, pas dans la RAM interne */
static void *big(size_t n) { void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return p ? p : calloc(1, n); }
#else
static void *big(size_t n) { return calloc(1, n); }
#endif

static const char *const COLORS[RC_COLOR_N] = { "grey", "green", "red", "blue", "yellow", "orange", "purple" };
rc_set_t *rc_alloc(void) { return big(sizeof(rc_set_t)); }
const char *rc_color_name(int c) { return c >= 0 && c < RC_COLOR_N ? COLORS[c] : "grey"; }

static int color_parse(const char *s)
{
    if (!s) return RC_GREY;
    for (int i = 0; i < RC_COLOR_N; i++) if (!strcmp(s, COLORS[i])) return i;
    if (!strcmp(s, "gray")) return RC_GREY;
    return RC_GREY;
}

bool rc_key_code(const rc_remote_t *r, const rc_key_t *k, ir_code_t *out)
{
    if (!k->has || !k->label[0]) return false;
    if (k->own) { out->proto = k->proto; out->addr = k->addr; }
    else {
        if (r->proto < 0 || !r->has_addr) return false;
        out->proto = (uint8_t)r->proto;
        out->addr = r->addr;
    }
    out->cmd = k->cmd;
    return true;
}

/* « 45 », « 0x45 » ou un nombre ; ndig : chiffres lus (adresse NEC) */
static bool hex_get(const cJSON *v, uint32_t *out, int *ndig)
{
    if (ndig) *ndig = 0;
    if (cJSON_IsNumber(v)) { if (v->valuedouble < 0) return false; *out = (uint32_t)v->valuedouble; if (ndig) *ndig = *out > 0xFF ? 4 : 2; return true; }
    if (!cJSON_IsString(v)) return false;
    const char *s = v->valuestring;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return false;
    uint32_t x = 0;
    int n = 0;
    for (; *s && *s != ' '; s++, n++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' : isxdigit((unsigned char)*s) ? (tolower((unsigned char)*s) - 'a' + 10) : -1;
        if (d < 0 || n >= 6) return false;
        x = x * 16 + (uint32_t)d;
    }
    *out = x;
    if (ndig) *ndig = n;
    return true;
}

static uint16_t addr_norm(int proto, uint32_t a, int ndig)
{
    if (proto == IR_NEC && ndig <= 2) return ir_nec_std((uint8_t)a);   /* adresse standard */
    return (uint16_t)a;
}

int rc_parse(const char *json, rc_set_t *set, const char **err)
{
    static const char *e_dummy;
    if (!err) err = &e_dummy;
    memset(set, 0, sizeof *set);
    cJSON *root = cJSON_Parse(json);
    if (!root) { *err = "json"; return -1; }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "remotes");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); *err = "remotes"; return -1; }
    int rc = 0;
    const cJSON *jr;
    cJSON_ArrayForEach(jr, arr) {
        if (set->n >= RC_MAX_REMOTES) { *err = "too_many"; rc = -1; break; }
        rc_remote_t *r = &set->r[set->n];
        memset(r, 0, sizeof *r);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(jr, "name");
        snprintf(r->name, sizeof r->name, "%s", cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : "?");
        v = cJSON_GetObjectItemCaseSensitive(jr, "proto");
        r->proto = (int8_t)(cJSON_IsString(v) ? ir_proto_parse(v->valuestring) : -1);
        if (cJSON_IsString(v) && r->proto < 0 && strcmp(v->valuestring, "auto")) { *err = "proto"; rc = -1; break; }
        uint32_t x; int nd;
        v = cJSON_GetObjectItemCaseSensitive(jr, "addr");
        if (v && !cJSON_IsNull(v)) {
            if (!hex_get(v, &x, &nd) || x > 0xFFFF) { *err = "addr"; rc = -1; break; }
            r->has_addr = true;
            r->addr = addr_norm(r->proto, x, nd);
        }
        v = cJSON_GetObjectItemCaseSensitive(jr, "cols");
        r->cols = (uint8_t)(cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= 4 ? v->valueint : 3);
        const cJSON *keys = cJSON_GetObjectItemCaseSensitive(jr, "keys");
        if (!cJSON_IsArray(keys)) { *err = "keys"; rc = -1; break; }
        const cJSON *jk;
        cJSON_ArrayForEach(jk, keys) {
            if (r->nkeys >= RC_MAX_KEYS) { *err = "too_many_keys"; rc = -1; break; }
            rc_key_t *k = &r->keys[r->nkeys];
            memset(k, 0, sizeof *k);
            v = cJSON_GetObjectItemCaseSensitive(jk, "label");
            snprintf(k->label, sizeof k->label, "%s", cJSON_IsString(v) ? v->valuestring : "");
            v = cJSON_GetObjectItemCaseSensitive(jk, "color");
            k->color = (uint8_t)color_parse(cJSON_IsString(v) ? v->valuestring : NULL);
            v = cJSON_GetObjectItemCaseSensitive(jk, "w");
            k->w = (uint8_t)(cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= r->cols ? v->valueint : 1);
            k->repeat = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(jk, "repeat"));
            int kp = r->proto;
            v = cJSON_GetObjectItemCaseSensitive(jk, "proto");
            if (cJSON_IsString(v)) {
                kp = ir_proto_parse(v->valuestring);
                if (kp < 0) { *err = "proto"; rc = -1; break; }
                k->own = true;
            }
            k->proto = (uint8_t)(kp < 0 ? 0 : kp);
            v = cJSON_GetObjectItemCaseSensitive(jk, "addr");
            if (v && !cJSON_IsNull(v)) {
                if (!hex_get(v, &x, &nd) || x > 0xFFFF) { *err = "addr"; rc = -1; break; }
                k->own = true;
                k->addr = addr_norm(kp, x, nd);
            } else if (k->own) {
                k->addr = r->addr;                         /* protocole propre, adresse héritée */
            }
            v = cJSON_GetObjectItemCaseSensitive(jk, "code");
            if (v && !cJSON_IsNull(v)) {
                if (!hex_get(v, &x, NULL) || x > 0xFFFF) { *err = "code"; rc = -1; break; }
                k->has = true;
                k->cmd = (uint16_t)x;
            }
            r->nkeys++;
        }
        if (rc) break;
        set->n++;
    }
    cJSON_Delete(root);
    if (rc == 0 && set->n == 0) { *err = "empty"; rc = -1; }
    return rc;
}

static void put_hex(cJSON *o, const char *key, uint32_t v, int digits)
{
    char b[8];
    snprintf(b, sizeof b, digits == 4 ? "%04X" : "%02X", (unsigned)v);
    cJSON_AddStringToObject(o, key, b);
}

static void put_addr(cJSON *o, int proto, uint16_t a)
{
    if (proto == IR_NEC && ir_nec_is_std(a)) put_hex(o, "addr", a & 0xFF, 2);
    else put_hex(o, "addr", a, a > 0xFF ? 4 : 2);
}

char *rc_to_json(const rc_set_t *set, bool pretty)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "remotes");
    for (int i = 0; i < set->n; i++) {
        const rc_remote_t *r = &set->r[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", r->name);
        cJSON_AddStringToObject(o, "proto", r->proto < 0 ? "auto" : ir_proto_name(r->proto));
        if (r->has_addr) put_addr(o, r->proto, r->addr);
        cJSON_AddNumberToObject(o, "cols", r->cols);
        cJSON *keys = cJSON_AddArrayToObject(o, "keys");
        for (int j = 0; j < r->nkeys; j++) {
            const rc_key_t *k = &r->keys[j];
            cJSON *ko = cJSON_CreateObject();
            cJSON_AddStringToObject(ko, "label", k->label);
            if (k->label[0]) {
                if (k->own) {
                    cJSON_AddStringToObject(ko, "proto", ir_proto_name(k->proto));
                    put_addr(ko, k->proto, k->addr);
                }
                if (k->has) put_hex(ko, "code", k->cmd, k->cmd > 0xFF ? 4 : 2);
                if (k->color != RC_GREY) cJSON_AddStringToObject(ko, "color", COLORS[k->color]);
                if (k->repeat) cJSON_AddTrueToObject(ko, "repeat");
            }
            if (k->w > 1) cJSON_AddNumberToObject(ko, "w", k->w);
            cJSON_AddItemToArray(keys, ko);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *s = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ------------------------------------------------------------------ modèles */
static const char *const PRESET_NAME[RC_PRESET_N] = {
    N_("Lampe solaire"), N_("TV Philips"), N_("TV Samsung"), N_("TV LG"), N_("TV Sony"), N_("Car MP3 (21 touches)"),
};

/* les libellés en français sont traduits à l'ajout (i18n) */
#define TV6(n, p, a, pw, mu, vp, cp, vm, cm) \
    "{\"name\":\"" n "\",\"proto\":\"" p "\",\"addr\":\"" a "\",\"cols\":2,\"keys\":[" \
    "{\"label\":\"Veille\",\"code\":\"" pw "\",\"color\":\"red\"},{\"label\":\"Muet\",\"code\":\"" mu "\"}," \
    "{\"label\":\"Vol +\",\"code\":\"" vp "\",\"color\":\"blue\",\"repeat\":true},{\"label\":\"Prog +\",\"code\":\"" cp "\",\"color\":\"green\"}," \
    "{\"label\":\"Vol -\",\"code\":\"" vm "\",\"color\":\"blue\",\"repeat\":true},{\"label\":\"Prog -\",\"code\":\"" cm "\",\"color\":\"green\"}]}"

static const char *const PRESET_JSON[RC_PRESET_N] = {
    "{\"name\":\"Lampe solaire\",\"proto\":\"nec\",\"addr\":\"00\",\"cols\":3,\"keys\":["
    "{\"label\":\"ON\",\"code\":\"45\",\"color\":\"green\"},{\"label\":\"\"},{\"label\":\"OFF\",\"code\":\"47\",\"color\":\"red\"},"
    "{\"label\":\"3H\",\"code\":\"07\",\"color\":\"blue\"},{\"label\":\"5H\",\"code\":\"15\",\"color\":\"blue\"},{\"label\":\"8H\",\"code\":\"09\",\"color\":\"blue\"},"
    "{\"label\":\"25 %\",\"code\":\"0C\",\"color\":\"yellow\"},{\"label\":\"50 %\",\"code\":\"18\",\"color\":\"yellow\"},{\"label\":\"100 %\",\"code\":\"5E\",\"color\":\"yellow\"}]}",
    TV6("TV Philips", "rc6", "00", "0C", "0D", "10", "20", "11", "21"),
    TV6("TV Samsung", "samsung", "07", "02", "0F", "07", "12", "0B", "10"),
    TV6("TV LG", "nec", "04", "08", "09", "02", "00", "03", "01"),
    TV6("TV Sony", "sony12", "01", "15", "14", "12", "10", "13", "11"),
    "{\"name\":\"Car MP3\",\"proto\":\"nec\",\"addr\":\"00\",\"cols\":3,\"keys\":["
    "{\"label\":\"CH-\",\"code\":\"45\"},{\"label\":\"CH\",\"code\":\"46\"},{\"label\":\"CH+\",\"code\":\"47\"},"
    "{\"label\":\"|<<\",\"code\":\"44\"},{\"label\":\">>|\",\"code\":\"40\"},{\"label\":\"> ||\",\"code\":\"43\",\"color\":\"green\"},"
    "{\"label\":\"-\",\"code\":\"07\",\"color\":\"blue\",\"repeat\":true},{\"label\":\"+\",\"code\":\"15\",\"color\":\"blue\",\"repeat\":true},{\"label\":\"EQ\",\"code\":\"09\"},"
    "{\"label\":\"0\",\"code\":\"16\"},{\"label\":\"100+\",\"code\":\"19\"},{\"label\":\"200+\",\"code\":\"0D\"},"
    "{\"label\":\"1\",\"code\":\"0C\"},{\"label\":\"2\",\"code\":\"18\"},{\"label\":\"3\",\"code\":\"5E\"},"
    "{\"label\":\"4\",\"code\":\"08\"},{\"label\":\"5\",\"code\":\"1C\"},{\"label\":\"6\",\"code\":\"5A\"},"
    "{\"label\":\"7\",\"code\":\"42\"},{\"label\":\"8\",\"code\":\"52\"},{\"label\":\"9\",\"code\":\"4A\"}]}",
};

const char *rc_preset_name(int i) { return i >= 0 && i < RC_PRESET_N ? PRESET_NAME[i] : "?"; }
const char *rc_preset_json(int i) { return i >= 0 && i < RC_PRESET_N ? PRESET_JSON[i] : NULL; }

int rc_add_preset(rc_set_t *set, int i)
{
    if (i < 0 || i >= RC_PRESET_N || set->n >= RC_MAX_REMOTES) return -1;
    size_t l = strlen(PRESET_JSON[i]) + 32;
    char *j = malloc(l);
    if (!j) return -1;
    snprintf(j, l, "{\"remotes\":[%s]}", PRESET_JSON[i]);
    rc_set_t *tmp = big(sizeof *tmp);
    int rc = tmp ? rc_parse(j, tmp, NULL) : -1;
    free(j);
    if (rc || tmp->n != 1) { free(tmp); return -1; }
    rc_remote_t *r = &tmp->r[0];
    snprintf(r->name, sizeof r->name, "%s", _(PRESET_NAME[i]));
    for (int k = 0; k < r->nkeys; k++) {
        char l[RC_LABEL_LEN];                               /* _() peut rendre le même pointeur */
        memcpy(l, r->keys[k].label, sizeof l);
        snprintf(r->keys[k].label, sizeof r->keys[k].label, "%s", _(l));
    }
    set->r[set->n++] = *r;
    free(tmp);
    return 0;
}

/* ------------------------------------------------------------------ NVS */
#ifndef RC_NO_NVS
#define NS "remote"
#define KEY "json"
#define JSON_MAX 12000

char *rc_load_json(bool pretty)
{
    rc_set_t *set = big(sizeof *set);
    if (!set) return NULL;
    rc_load(set);
    char *s = rc_to_json(set, pretty);
    free(set);
    return s;
}

static char *nvs_json(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t n = 0;
    char *s = NULL;
    if (nvs_get_blob(h, KEY, NULL, &n) == ESP_OK && n > 0 && n < JSON_MAX) {
        s = malloc(n + 1);
        if (s && nvs_get_blob(h, KEY, s, &n) == ESP_OK) s[n] = 0;
        else { free(s); s = NULL; }
    }
    nvs_close(h);
    return s;
}

/* codes de l'ancienne app « Lampe IR » (0.32), repris une fois */
typedef struct { uint8_t ver, valid; uint16_t addr[8]; uint8_t cmd[8]; } old_keys_t;

static void defaults(rc_set_t *set)
{
    memset(set, 0, sizeof *set);
    rc_add_preset(set, 0);                                  /* lampe solaire */
    rc_add_preset(set, 1);                                  /* TV Philips */
    nvs_handle_t h;
    old_keys_t o;
    size_t n = sizeof o;
    if (nvs_open("irlamp", NVS_READONLY, &h) != ESP_OK) return;
    bool ok = nvs_get_blob(h, "keys", &o, &n) == ESP_OK && n == sizeof o && o.ver == 1 && o.valid;
    nvs_close(h);
    if (!ok) return;
    /* ordre de l'ancienne app : ON OFF 3H 5H 8H 25 % 50 % 100 % ; ici une case vide après ON */
    static const int8_t MAP[8] = { 0, 2, 3, 4, 5, 6, 7, 8 };
    rc_remote_t *r = &set->r[0];
    uint16_t a = 0;
    for (int k = 0; k < 8; k++) if (o.valid & (1 << k)) a = o.addr[k];
    if (a != r->addr) {                                     /* autre lampe : codes du modèle inconnus */
        r->addr = a;
        for (int k = 0; k < r->nkeys; k++) r->keys[k].has = false;
    }
    for (int k = 0; k < 8; k++)
        if (o.valid & (1 << k)) { r->keys[MAP[k]].has = true; r->keys[MAP[k]].cmd = o.cmd[k]; }
}

void rc_load(rc_set_t *set)
{
    char *s = nvs_json();
    if (s && rc_parse(s, set, NULL) == 0) { free(s); return; }
    free(s);
    defaults(set);
}

static int nvs_put(const char *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    int rc = nvs_set_blob(h, KEY, s, strlen(s)) == ESP_OK && nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
}

int rc_save(const rc_set_t *set)
{
    char *s = rc_to_json(set, false);
    if (!s) return -1;
    int rc = strlen(s) < JSON_MAX ? nvs_put(s) : -1;
    cJSON_free(s);
    return rc;
}

int rc_save_json(const char *json, const char **err)
{
    rc_set_t *set = big(sizeof *set);
    if (!set) { *err = "nvs"; return -1; }
    int rc = rc_parse(json, set, err);
    if (rc == 0 && rc_save(set)) { *err = "nvs"; rc = -1; }
    free(set);
    return rc;
}
#endif
