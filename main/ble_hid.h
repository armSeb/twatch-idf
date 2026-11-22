/*
 * ble_hid.h — la montre en souris / clavier / télécommande multimédia
 * Bluetooth (HID over GATT), pour piloter un ordinateur.
 *
 * Le mode souris est une autre identité Bluetooth : ble_set_hid_mode(true)
 * puis ble_start() redémarre la pile avec une autre adresse, le nom
 * « T-Watch Souris » et les services HID, sans le service de notifications
 * de l'iPhone (qui ne la voit donc pas comme une souris). On revient à la
 * montre normale par ble_set_hid_mode(false) + ble_start().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HID_NAME "T-Watch Souris"

/* boutons de la souris */
enum { HID_BTN_LEFT = 1, HID_BTN_RIGHT = 2, HID_BTN_MIDDLE = 4 };

/* touches clavier (usages HID page 0x07) */
enum {
    HID_KEY_B = 0x05, HID_KEY_ENTER = 0x28, HID_KEY_ESC = 0x29, HID_KEY_SPACE = 0x2C,
    HID_KEY_F5 = 0x3E, HID_KEY_HOME = 0x4A, HID_KEY_PGUP = 0x4B, HID_KEY_END = 0x4D, HID_KEY_PGDN = 0x4E,
    HID_KEY_RIGHT = 0x4F, HID_KEY_LEFT = 0x50, HID_KEY_DOWN = 0x51, HID_KEY_UP = 0x52,
};
/* touches multimédia (usages HID page 0x0C) */
enum {
    HID_CC_PLAY = 0xCD, HID_CC_NEXT = 0xB5, HID_CC_PREV = 0xB6, HID_CC_STOP = 0xB7,
    HID_CC_MUTE = 0xE2, HID_CC_VOL_UP = 0xE9, HID_CC_VOL_DOWN = 0xEA,
};

bool hid_ready(void);                     /* connecté, chiffré, abonné aux rapports */
bool hid_connected(void);
/* thread-safe ; renvoient false si rien n'est parti (pas prêt) */
bool hid_mouse(uint8_t buttons, int dx, int dy, int wheel);
bool hid_key(uint8_t mod, uint8_t key);   /* appui puis relâché */
bool hid_consumer(uint16_t usage);        /* appui puis relâché */

/* usage interne (ble.c) */
struct ble_gatt_svc_def;
const struct ble_gatt_svc_def *hid_svcs(void);
void hid_on_connect(uint16_t conn);
void hid_on_encrypted(void);
void hid_on_disconnect(void);
void hid_on_subscribe(uint16_t attr, bool notify);
