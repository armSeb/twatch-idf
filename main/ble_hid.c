/* ble_hid.c — voir ble_hid.h */
#include "ble_hid.h"
#include <string.h>
#include "ble.h"
#include "host/ble_hs.h"

/* Descripteur de rapports : souris (ID 1, 3 boutons, X, Y, molette,
 * défilement horizontal), clavier (ID 2), multimédia (ID 3). */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,       /* souris */
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,       /* clavier */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,       /* multimédia */
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

enum { R_MOUSE, R_KBD, R_CC, R_N };
static const uint8_t REF[R_N][2] = { { 1, 1 }, { 2, 1 }, { 3, 1 } };   /* ID, type 1 = entrée */
static uint16_t s_h[R_N];                 /* handles des valeurs */
static uint16_t s_batt_h;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_enc;
static volatile uint8_t s_sub;            /* un bit par rapport abonné */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    intptr_t what = (intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;           /* point de contrôle */
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    static const uint8_t INFO[] = { 0x11, 0x01, 0x00, 0x02 };          /* HID 1.11, normalement connectable */
    static const uint8_t PNP[] = { 0x02, 0x3A, 0x30, 0x20, 0x20, 0x00, 0x01 };   /* USB, Espressif, produit, v1 */
    static const uint8_t ZERO[8];
    int rc;
    switch (what) {
    case 1: rc = os_mbuf_append(ctxt->om, INFO, sizeof INFO); break;
    case 2: rc = os_mbuf_append(ctxt->om, REPORT_MAP, sizeof REPORT_MAP); break;
    case 3: rc = os_mbuf_append(ctxt->om, ZERO, 5); break;            /* rapport souris */
    case 4: rc = os_mbuf_append(ctxt->om, ZERO, 8); break;            /* rapport clavier */
    case 5: rc = os_mbuf_append(ctxt->om, ZERO, 2); break;            /* rapport multimédia */
    case 6: rc = os_mbuf_append(ctxt->om, PNP, sizeof PNP); break;
    case 7: rc = os_mbuf_append(ctxt->om, "LilyGo", 6); break;
    case 8: { uint8_t b = ble_get_battery(); rc = os_mbuf_append(ctxt->om, &b, 1); break; }
    case 9: { uint8_t p = 1; rc = os_mbuf_append(ctxt->om, &p, 1); break; }   /* mode rapports */
    default: return BLE_ATT_ERR_UNLIKELY;
    }
    return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int ref_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint8_t *r = arg;
    return os_mbuf_append(ctxt->om, r, 2) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

#define F_RD (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define REPORT(i, what) { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .arg = (void *)(what), \
    .flags = F_RD | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h[i], \
    .descriptors = (struct ble_gatt_dsc_def[]) { { .uuid = BLE_UUID16_DECLARE(0x2908), \
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = ref_access, .arg = (void *)REF[i] }, { 0 } } }

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = chr_access, .arg = (void *)1, .flags = F_RD },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = chr_access, .arg = (void *)2, .flags = F_RD },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = chr_access, .arg = (void *)0,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = chr_access, .arg = (void *)9,
              .flags = F_RD | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
            REPORT(R_MOUSE, 3),
            REPORT(R_KBD, 4),
            REPORT(R_CC, 5),
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = chr_access, .arg = (void *)6, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = chr_access, .arg = (void *)7, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = chr_access, .arg = (void *)8,
              .flags = F_RD | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_h },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def *hid_svcs(void) { return SVCS; }

void hid_on_connect(uint16_t conn) { s_conn = conn; s_enc = false; s_sub = 0; }
void hid_on_encrypted(void) { s_enc = true; }
void hid_on_disconnect(void) { s_conn = BLE_HS_CONN_HANDLE_NONE; s_enc = false; s_sub = 0; }

void hid_on_subscribe(uint16_t attr, bool notify)
{
    for (int i = 0; i < R_N; i++)
        if (attr == s_h[i]) s_sub = notify ? s_sub | (1 << i) : s_sub & ~(1 << i);
}

bool hid_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
/* Windows et Linux s'abonnent aux trois rapports ; macOS parfois seulement à
 * celui de la souris : on se contente d'un abonnement quelconque */
bool hid_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_enc && s_sub; }

static bool send(int i, const void *d, int n)
{
    uint16_t c = s_conn;
    if (c == BLE_HS_CONN_HANDLE_NONE || !s_enc) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)n);
    if (!om) return false;
    return ble_gatts_notify_custom(c, s_h[i], om) == 0;
}

static int8_t clamp8(int v) { return (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v); }

bool hid_mouse(uint8_t buttons, int dx, int dy, int wheel)
{
    bool ok = true;
    do {                                                  /* grands déplacements : en plusieurs rapports */
        int8_t x = clamp8(dx), y = clamp8(dy), w = clamp8(wheel);
        uint8_t r[5] = { buttons, (uint8_t)x, (uint8_t)y, (uint8_t)w, 0 };
        ok &= send(R_MOUSE, r, 5);
        dx -= x; dy -= y; wheel -= w;
    } while (ok && (dx || dy || wheel));
    return ok;
}

bool hid_key(uint8_t mod, uint8_t key)
{
    uint8_t r[8] = { mod, 0, key };
    bool ok = send(R_KBD, r, 8);
    memset(r, 0, sizeof r);
    return send(R_KBD, r, 8) && ok;
}

bool hid_consumer(uint16_t usage)
{
    uint8_t r[2] = { (uint8_t)usage, (uint8_t)(usage >> 8) };
    bool ok = send(R_CC, r, 2);
    r[0] = r[1] = 0;
    return send(R_CC, r, 2) && ok;
}
