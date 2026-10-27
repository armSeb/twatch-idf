/*
 * Tests PC de la logique pure : réassemblage ANCS, nettoyage UTF-8,
 * déduplication du stockage. Lancer : ./run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ancs_types.h"
#include "notif_store.h"

int64_t g_fake_time_us;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static size_t put_attr(uint8_t *b, uint8_t id, const char *s)
{
    size_t n = strlen(s);
    b[0] = id; b[1] = n & 0xFF; b[2] = n >> 8;
    memcpy(b + 3, s, n);
    return 3 + n;
}

static size_t build_notif(uint8_t *b, uint32_t uid, const char *app, const char *title,
                          const char *msg, const char *date, int shuffle)
{
    size_t p = 0;
    b[p++] = 0;
    b[p++] = uid; b[p++] = uid >> 8; b[p++] = uid >> 16; b[p++] = uid >> 24;
    if (shuffle) {
        p += put_attr(b + p, 7, "Effacer");
        p += put_attr(b + p, 3, msg);
        p += put_attr(b + p, 0, app);
        p += put_attr(b + p, 5, date);
        p += put_attr(b + p, 1, title);
        p += put_attr(b + p, 6, "");
    } else {
        p += put_attr(b + p, 0, app);
        p += put_attr(b + p, 1, title);
        p += put_attr(b + p, 3, msg);
        p += put_attr(b + p, 5, date);
        p += put_attr(b + p, 6, "");
        p += put_attr(b + p, 7, "Effacer");
    }
    return p;
}

static void test_parser_fragments(void)
{
    uint8_t full[1024];
    const char *msg = "Salut ! On se retrouve à 18h ? Prends l’appareil 📷 stp…";
    for (int shuffle = 0; shuffle < 2; shuffle++) {
        size_t len = build_notif(full, 0xA1B2C3D4, "net.whatsapp.WhatsApp", "Élodie", msg,
                                 "20261027T101500", shuffle);
        for (int iter = 0; iter < 5000; iter++) {
            ancs_notif_t n;
            memset(&n, 0, sizeof(n));
            size_t got = 0;
            int r = 0;
            while (got < len) {
                size_t chunk = 1 + rand() % 40;
                if (got + chunk > len) chunk = len - got;
                got += chunk;
                r = ancs_parse_notif_attrs(full, got, 0xA1B2C3D4, 6, &n);
                if (got < len) CHECK(r == 0);
                if (r != 0) break;
            }
            CHECK(r == 1);
            CHECK(strcmp(n.app_id, "net.whatsapp.WhatsApp") == 0);
            CHECK(strcmp(n.title, "Élodie") == 0);
            CHECK(strcmp(n.message, msg) == 0);
            CHECK(strcmp(n.date, "20261027T101500") == 0);
            CHECK(strcmp(n.neg_label, "Effacer") == 0);
            CHECK(n.pos_label[0] == 0);
            if (fails) return;
        }
    }
    /* mauvais UID / mauvaise commande */
    ancs_notif_t n;
    size_t len = build_notif(full, 42, "a", "b", "c", "d", 0);
    CHECK(ancs_parse_notif_attrs(full, len, 43, 6, &n) == -1);
    full[0] = 1;
    CHECK(ancs_parse_notif_attrs(full, len, 42, 6, &n) == -1);
    /* AppIdentifier plus long que le tampon : tronqué proprement */
    char longid[200];
    memset(longid, 'x', sizeof(longid) - 1); longid[199] = 0;
    len = build_notif(full, 7, longid, "t", "m", "20261027T101500", 0);
    memset(&n, 0, sizeof(n));
    CHECK(ancs_parse_notif_attrs(full, len, 7, 6, &n) == 1);
    CHECK(strlen(n.app_id) == ANCS_APPID_MAX);
    printf("parser fragments : ok\n");
}

static void test_app_attrs(void)
{
    uint8_t b[128];
    size_t p = 0;
    b[p++] = 1;
    memcpy(b + p, "com.apple.MobileSMS", 19); p += 19;
    b[p++] = 0;
    p += put_attr(b + p, 0, "Messages");
    for (size_t cut = 0; cut < p; cut++) {
        char name[33] = "";
        CHECK(ancs_parse_app_attrs(b, cut, name, sizeof(name)) == 0);
    }
    char name[33] = "";
    CHECK(ancs_parse_app_attrs(b, p, name, sizeof(name)) == 1);
    CHECK(strcmp(name, "Messages") == 0);
    printf("app attrs : ok\n");
}

static void test_sanitize(void)
{
    char s[256];
    strcpy(s, "  Café\r\n\r\n\r\n\r\nl’été 😀👍🏽 à 20 €\xC2\xA0ok\xE2\x80");   /* coupure UTF-8 finale */
    text_sanitize(s);
    CHECK(strcmp(s, "Café\n\nl’été à 20 € ok") == 0);
    strcpy(s, "\xFF\xFE ABC \xE2\x80\xA8 def");
    text_sanitize(s);
    CHECK(strcmp(s, "ABC \ndef") == 0 || strcmp(s, "ABC\ndef") == 0);
    strcpy(s, "Œuvre « citée » — fin");
    text_sanitize(s);
    CHECK(strcmp(s, "Œuvre « citée » — fin") == 0);
    strcpy(s, "Prends le fer à souder 🔥, l’étain");
    text_sanitize(s);
    CHECK(strcmp(s, "Prends le fer à souder, l’étain") == 0);
    printf("sanitize : ok\n");
}

static ancs_notif_t mk(uint32_t uid, const char *title, const char *date, uint8_t flags)
{
    ancs_notif_t n;
    memset(&n, 0, sizeof(n));
    n.uid = uid;
    n.flags = flags;
    strcpy(n.app_id, "com.apple.MobileSMS");
    strcpy(n.app_name, "Messages");
    strcpy(n.title, title);
    strcpy(n.message, "corps");
    strcpy(n.date, date);
    return n;
}

static void test_store(void)
{
    bool alert;
    ns_clear();
    g_fake_time_us = 1000000;
    ancs_notif_t a = mk(10, "Alice", "20261027T100000", 0);
    ns_add(&a, &alert);
    CHECK(alert && ns_count() == 1);

    /* reconnexion : replay iOS (PreExisting) du même contenu avec un nouvel UID */
    ns_invalidate_uids();
    ancs_notif_t a2 = mk(99, "Alice", "20261027T100000", ANCS_FLAG_PREEXISTING);
    ns_add(&a2, &alert);
    CHECK(!alert && ns_count() == 1);
    CHECK(ns_find_uid(99) != NULL && ns_find_uid(10) == NULL);

    /* doublon immédiat non PreExisting : fusion silencieuse */
    ancs_notif_t a3 = mk(100, "Alice", "20261027T100000", 0);
    ns_add(&a3, &alert);
    CHECK(!alert && ns_count() == 1);

    /* même contenu 6 minutes plus tard : vraie nouveauté */
    g_fake_time_us += 6LL * 60 * 1000000;
    ancs_notif_t a4 = mk(101, "Alice", "20261027T100600", 0);
    ns_add(&a4, &alert);
    CHECK(alert && ns_count() == 1);

    /* tri par date : plus ancienne (pré-existante) en bas */
    ancs_notif_t b = mk(11, "Bob", "20261026T080000", ANCS_FLAG_PREEXISTING);
    ns_add(&b, &alert);
    CHECK(!alert && ns_count() == 2 && strcmp(ns_get(1)->n.title, "Bob") == 0);
    ancs_notif_t c = mk(12, "Chloé", "20261027T110000", 0);
    ns_add(&c, &alert);
    CHECK(strcmp(ns_get(0)->n.title, "Chloé") == 0);

    /* Modified : remplace le contenu de l'UID */
    ancs_notif_t c2 = mk(12, "Chloé", "20261027T110000", 0);
    strcpy(c2.message, "corps modifié");
    c2.modified = true;
    ns_add(&c2, &alert);
    CHECK(!alert && ns_count() == 3 && strcmp(ns_find_uid(12)->n.message, "corps modifié") == 0);

    /* Removed */
    CHECK(ns_remove_uid(11) && ns_count() == 2);
    CHECK(!ns_remove_uid(11));

    /* capacité */
    ns_clear();
    for (int i = 0; i < 30; i++) {
        char t[16], d[20];
        snprintf(t, sizeof(t), "N%d", i);
        snprintf(d, sizeof(d), "20261027T10%02d00", i);
        ancs_notif_t n = mk(1000 + i, t, d, 0);
        ns_add(&n, &alert);
    }
    CHECK(ns_count() == NS_MAX);
    CHECK(strcmp(ns_get(0)->n.title, "N29") == 0);
    CHECK(strcmp(ns_get(NS_MAX - 1)->n.title, "N10") == 0);
    printf("store : ok\n");
}

int main(void)
{
    srand(1234);
    test_parser_fragments();
    test_app_attrs();
    test_sanitize();
    test_store();
    printf(fails ? "\n%d ÉCHEC(S)\n" : "\nTOUS LES TESTS PASSENT\n", fails);
    return fails ? 1 : 0;
}
