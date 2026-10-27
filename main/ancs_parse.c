/*
 * ancs_parse.c — décodage des réponses Data Source ANCS (pur C, sans NimBLE).
 * Testé sur PC par test/test_host.c (fragmentation aléatoire, troncatures).
 */
#include "ancs_types.h"
#include <string.h>

#define CMD_GET_NOTIF_ATTR  0
#define CMD_GET_APP_ATTR    1

static void copy_str(char *dst, size_t dsz, const uint8_t *src, size_t n)
{
    if (n > dsz - 1) n = dsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int ancs_parse_notif_attrs(const uint8_t *buf, size_t len, uint32_t uid, int nattrs, ancs_notif_t *n)
{
    if (len < 5) return 0;
    if (buf[0] != CMD_GET_NOTIF_ATTR) return -1;
    uint32_t ruid = buf[1] | (buf[2] << 8) | (buf[3] << 16) | ((uint32_t)buf[4] << 24);
    if (ruid != uid) return -1;
    size_t pos = 5;
    for (int found = 0; found < nattrs; found++) {
        if (pos + 3 > len) return 0;
        uint8_t id = buf[pos];
        size_t alen = buf[pos + 1] | (buf[pos + 2] << 8);
        if (pos + 3 + alen > len) return 0;
        const uint8_t *d = &buf[pos + 3];
        switch (id) {
        case 0: copy_str(n->app_id, sizeof(n->app_id), d, alen); break;
        case 1: copy_str(n->title, sizeof(n->title), d, alen); break;
        case 3: copy_str(n->message, sizeof(n->message), d, alen); break;
        case 5: copy_str(n->date, sizeof(n->date), d, alen); break;
        case 6: copy_str(n->pos_label, sizeof(n->pos_label), d, alen); break;
        case 7: copy_str(n->neg_label, sizeof(n->neg_label), d, alen); break;
        default: break;   /* attribut non demandé : ignoré mais compté */
        }
        pos += 3 + alen;
    }
    return 1;
}

int ancs_parse_app_attrs(const uint8_t *buf, size_t len, char *name, size_t name_sz)
{
    if (len < 1) return 0;
    if (buf[0] != CMD_GET_APP_ATTR) return -1;
    size_t pos = 1;
    while (pos < len && buf[pos] != 0) pos++;
    if (pos >= len) return 0;
    pos++;                                   /* NUL de l'AppIdentifier */
    if (pos + 3 > len) return 0;
    size_t alen = buf[pos + 1] | (buf[pos + 2] << 8);
    if (pos + 3 + alen > len) return 0;
    if (buf[pos] == 0 && alen > 0) copy_str(name, name_sz, &buf[pos + 3], alen);
    return 1;
}
