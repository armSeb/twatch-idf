#include "notif_store.h"
#include "esp_attr.h"
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "store";

EXT_RAM_BSS_ATTR static notif_entry_t s_e[NS_MAX];   /* 9,6 Ko : en PSRAM */
static int s_n;

/* ------------------------------------------------------------------ */
/* Nettoyage UTF-8                                                       */
/* iOS tronque les attributs à N octets (coupure possible au milieu     */
/* d'un caractère) et envoie des émojis / caractères invisibles absents */
/* de nos polices. On ne garde que ce que les polices savent dessiner.  */
/* ------------------------------------------------------------------ */
static bool cp_supported(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) return true;
    if (cp >= 0xA1 && cp <= 0xFF) return true;
    switch (cp) {
    case 0x152: case 0x153: case 0x178:
    case 0x2013: case 0x2014: case 0x2018: case 0x2019:
    case 0x201C: case 0x201D: case 0x201E: case 0x2022: case 0x2026: case 0x20AC:
        return true;
    default:
        return false;
    }
}

static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
    o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F);
    return 3;
}

void text_sanitize(char *s)
{
    const unsigned char *in = (const unsigned char *)s;
    char *out = s;
    int nl_run = 0;
    bool space_run = true;    /* supprime les blancs initiaux */
    bool dropped = false;     /* un caractère vient d'être retiré (émoji…) */
    while (*in) {
        uint32_t cp;
        int len;
        unsigned char b = *in;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { in++; continue; }                       /* octet invalide */
        bool ok = true;
        for (int i = 1; i < len; i++) {
            if ((in[i] & 0xC0) != 0x80) { ok = false; break; }   /* inclut le NUL final */
            cp = (cp << 6) | (in[i] & 0x3F);
        }
        if (!ok) { in++; continue; }
        in += len;

        if (cp == '\r') continue;
        if (cp == 0x2028 || cp == 0x2029) cp = '\n';
        if (cp == 0x00A0 || cp == '\t' || cp == 0x202F) cp = ' ';
        if (cp >= 0x2010 && cp <= 0x2012) cp = '-';
        if (cp == 0x2032) cp = '\'';

        if (cp == '\n') {
            if (++nl_run > 2) continue;
            *out++ = '\n';
            space_run = true;
            continue;
        }
        if (!cp_supported(cp)) { dropped = true; continue; }   /* émojis, ZWJ, U+FFFC… */
        /* « souder 🔥, » → « souder, » : pas d'espace orphelin avant la ponctuation */
        if (dropped && out > s && out[-1] == ' ' && cp < 0x80 && strchr(",.)", (int)cp)) out--;
        if (cp == ' ') {
            if (space_run) continue;
            space_run = true;
        } else {
            space_run = false;
        }
        nl_run = 0;
        if (cp != ' ') dropped = false;
        out += utf8_put(out, cp);
    }
    *out = 0;
    /* blancs finaux */
    while (out > s && (out[-1] == ' ' || out[-1] == '\n')) *--out = 0;
}

static uint32_t fnv1a(uint32_t h, const char *s)
{
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    h ^= '|'; h *= 16777619u;
    return h;
}

/* ------------------------------------------------------------------ */
static int find_hash(uint32_t h)
{
    for (int i = 0; i < s_n; i++) if (s_e[i].hash == h) return i;
    return -1;
}

static int find_uid(uint32_t uid)
{
    for (int i = 0; i < s_n; i++) if (s_e[i].uid_valid && s_e[i].n.uid == uid) return i;
    return -1;
}

static void remove_at(int i)
{
    if (i < 0 || i >= s_n) return;
    memmove(&s_e[i], &s_e[i + 1], (s_n - i - 1) * sizeof(s_e[0]));
    s_n--;
}

/* insertion triée par date iOS décroissante (format yyyyMMddTHHmmSS,
 * comparable lexicographiquement) ; sans date → en tête */
static int insert_pos(const char *date)
{
    if (!date[0]) return 0;
    for (int i = 0; i < s_n; i++) {
        if (!s_e[i].n.date[0] || strcmp(s_e[i].n.date, date) <= 0) return i;
    }
    return s_n;
}

notif_entry_t *ns_add(ancs_notif_t *n, bool *alert)
{
    *alert = false;
    text_sanitize(n->title);
    text_sanitize(n->message);
    text_sanitize(n->app_name);
    text_sanitize(n->pos_label);
    text_sanitize(n->neg_label);

    uint32_t h = fnv1a(fnv1a(fnv1a(2166136261u, n->app_id), n->title), n->message);
    int64_t now = esp_timer_get_time();

    /* 1. modification d'une notif connue (même UID) */
    int i = n->modified ? find_uid(n->uid) : -1;
    /* 2. même contenu déjà présent (replay iOS après reconnexion, doublons) */
    if (i < 0) i = find_hash(h);

    if (i >= 0) {
        notif_entry_t *e = &s_e[i];
        bool fresh_repeat = !(n->flags & ANCS_FLAG_PREEXISTING) && !n->modified &&
                            (now - e->rx_us) > (int64_t)NS_DEDUP_TTL_S * 1000000;
        e->n = *n;
        e->hash = h;
        e->uid_valid = true;
        if (fresh_repeat) {
            /* vrai nouveau message au contenu identique : on remonte et on alerte */
            notif_entry_t tmp = *e;
            tmp.rx_us = now;
            remove_at(i);
            memmove(&s_e[1], &s_e[0], s_n * sizeof(s_e[0]));
            s_e[0] = tmp;
            s_n++;
            *alert = true;
            return &s_e[0];
        }
        ESP_LOGD(TAG, "fusion UID %lu", (unsigned long)n->uid);
        return e;
    }

    /* 3. nouvelle entrée */
    if (s_n == NS_MAX) s_n--;                       /* on oublie la plus ancienne */
    int pos = insert_pos(n->date);
    if (pos >= NS_MAX) pos = NS_MAX - 1;
    if (pos > s_n) pos = s_n;
    memmove(&s_e[pos + 1], &s_e[pos], (s_n - pos) * sizeof(s_e[0]));
    s_e[pos].n = *n;
    s_e[pos].hash = h;
    s_e[pos].rx_us = now;
    s_e[pos].uid_valid = true;
    s_n++;
    *alert = !(n->flags & ANCS_FLAG_PREEXISTING);
    return &s_e[pos];
}

bool ns_remove_uid(uint32_t uid)
{
    int i = find_uid(uid);
    if (i < 0) return false;
    remove_at(i);
    return true;
}

void ns_remove_entry(const notif_entry_t *e)
{
    if (e >= s_e && e < s_e + s_n) remove_at(e - s_e);
}

notif_entry_t *ns_find_uid(uint32_t uid)
{
    int i = find_uid(uid);
    return i < 0 ? NULL : &s_e[i];
}

notif_entry_t *ns_find_hash(uint32_t hash)
{
    int i = find_hash(hash);
    return i < 0 ? NULL : &s_e[i];
}

void ns_invalidate_uids(void)
{
    for (int i = 0; i < s_n; i++) s_e[i].uid_valid = false;
}

void ns_clear(void) { s_n = 0; }
int ns_count(void) { return s_n; }
notif_entry_t *ns_get(int idx) { return (idx >= 0 && idx < s_n) ? &s_e[idx] : NULL; }
