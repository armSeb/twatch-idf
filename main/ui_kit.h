/*
 * ui_kit.h — briques d'interface partagées par l'UI système et les apps.
 *
 * Tout appel se fait depuis la tâche app (seule propriétaire de LVGL).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "fonts/fonts.h"

/* palette système */
#define C_BG    lv_color_hex(0x000000)
#define C_FG    lv_color_hex(0xF2F2F2)
#define C_DIM   lv_color_hex(0x8A8F98)
#define C_OFF   lv_color_hex(0x3A3D44)
#define C_CARD  lv_color_hex(0x1A1D23)
#define C_ACC   lv_color_hex(0x3B82F6)
#define C_OK    lv_color_hex(0x22C55E)
#define C_WARN  lv_color_hex(0xF59E0B)
#define C_BAD   lv_color_hex(0xEF4444)

/* icônes (Font Awesome, présentes dans font_text_16 et font_title_20) */
#define SYM_COMMENTS  "\xEF\x82\x86"   /* U+F086 */
#define SYM_EYE       "\xEF\x81\xAE"   /* U+F06E */
#define SYM_STOPWATCH "\xEF\x8B\xB2"   /* U+F2F2 */
#define SYM_HOURGLASS "\xEF\x89\x92"   /* U+F252 */
#define SYM_BULB      "\xEF\x83\xAB"   /* U+F0EB */
#define SYM_MOON      "\xEF\x86\x86"   /* U+F186 */
#define SYM_SLIDERS   "\xEF\x87\x9E"   /* U+F1DE */
#define SYM_GRID      "\xEF\x80\x8A"   /* U+F00A */

#define UI_ANIM_MS 140

/* écran plein (fond noir, gestes de navigation système déjà branchés) */
lv_obj_t *ui_screen(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user_data);
lv_obj_t *ui_container(lv_obj_t *parent);
void      ui_bubble(lv_obj_t *obj);          /* laisse remonter les gestes au parent */
void      ui_toast(const char *text);        /* message bref par-dessus n'importe quel écran */
/* en-tête « ‹ Titre » qui ramène à l'écran d'origine ; renvoie la hauteur occupée */
int       ui_header(lv_obj_t *scr, const char *title);
