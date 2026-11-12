# Writing an app, translating

[Français](../fr/apps-and-i18n.md) · English

## Apps

An app is one file, `main/app_xxx.c`, that defines a `const app_t`, plus one line in
`main/apps.c`. It then appears in the launcher.

```c
typedef struct app_s {
    const char *name;                 /* launcher label (French key, see i18n) */
    const char *icon;                 /* symbol from font_title_20 (ui_kit.h) */
    uint32_t    color;                /* icon disc colour, 0xRRGGBB */
    void (*create)(lv_obj_t *scr);    /* build the view in scr */
    void (*destroy)(void);            /* forget pointers to the view (optional) */
    void (*on_show)(void);            /* optional */
    void (*on_hide)(void);            /* optional */
    bool (*on_event)(int code);       /* app_post_event(); true = open the app, wake the screen */
    uint32_t screen_timeout_ms;       /* 0 = user setting */
    bool (*busy)(void);               /* true = keep the screen on this app across sleep/wake */
} app_t;
```

### Rules

- **The view is destroyed when the screen turns off** (to save RAM) and rebuilt when the
  app is opened again. Keep durable state in `static` variables — the *model* — never in
  LVGL objects. Reset your view pointers in `destroy()`.
- The system gives you an empty screen on which swipe → already goes back to where the app
  was opened from. `ui_header(scr, title)` adds the standard « ‹ Title » header (40 px).
- Everything runs on the app task (the only LVGL owner). From another context (an
  `esp_timer` callback, a task), call `app_post_event(&app_xxx, code)`: the system delivers
  it to `on_event(code)` on the app task, even if the app is closed. Return `true` to have
  the app opened and the screen woken (alarm).
- Periodic refresh: create an `lv_timer` in `create()`, pause it in `on_hide()`, resume it in
  `on_show()`, delete it in `destroy()`.
- The open app is kept across sleep: the screen turns off *on* it (`on_hide()`) and wakes back
  to it (`on_show()`). Stop anything costly (radio, sensors) in `on_hide()`. `busy()` is
  informative (e.g. an app that is working).

### Building blocks (`ui_kit.h`)

Palette `C_BG`, `C_FG`, `C_DIM`, `C_OFF`, `C_CARD`, `C_ACC`, `C_OK`, `C_WARN`, `C_BAD`;
icons `SYM_*`; `ui_label`, `ui_button`, `ui_container`, `ui_segmented`, `ui_header`,
`ui_toast`, `ui_bubble` (lets gestures reach the screen through a child object).

### Minimal example

```c
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"

static int s_count;                         /* model: survives the view */
static lv_obj_t *v_lbl;                     /* view */

static void tap_cb(lv_event_t *e)
{
    lv_label_set_text_fmt(v_lbl, "%d", ++s_count);
}

static void create(lv_obj_t *scr)
{
    ui_header(scr, _("Compteur"));
    v_lbl = ui_label(scr, &font_num_44, C_FG, "");
    lv_obj_center(v_lbl);
    lv_label_set_text_fmt(v_lbl, "%d", s_count);
    lv_obj_t *b = ui_button(scr, "+1", C_ACC, tap_cb, NULL);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void destroy(void) { v_lbl = NULL; }

const app_t app_counter = {
    .name = N_("Compteur"), .icon = SYM_GRID, .color = 0x8B5CF6,
    .create = create, .destroy = destroy,
};
```

Then in `main/apps.c`: `extern const app_t app_counter;` and `&app_counter,` in `g_apps[]`,
and the English translation of `"Compteur"` in `main/i18n.c`.

Bundled apps worth reading: `app_chrono.c` (model/view, `busy`), `app_timer.c` (`esp_timer`
alarm with the screen off, `on_event`), `app_lamp.c` (brightness on show/hide),
`app_tvoff.c` (work in a separate task, progress).

## Translating

The mechanism follows gettext, without tools: **the French string is the key**.

- In code: `_("Réglages")` returns the string in the current language.
- In static tables, mark strings with `N_("...")` (no-op) and translate when displaying:
  `_(table[i])`.
- `main/i18n.c` holds the French → English table. A missing entry falls back to French.
- printf formats keep the same specifiers in the same order in both languages
  (`"Tour %d   %s"` → `"Lap %d   %s"`).
- `host_tests/check_i18n.py` (run by `host_tests/run.sh` and the CI) fails on any `_()`/`N_()`
  string without a translation or with mismatching formats, and lists unused entries.
- Changing the language (*Settings › Display › Language*) rebuilds all screens in place.

### Adding a language

1. Add `LANG_XX` to the enum in `main/i18n.h`.
2. Add a second table in `main/i18n.c` and pick it in `i18n_tr()`.
3. Add the language to the selector in `build_menu()` (`main/ui.c`) and accept the value in
   `settings.c`.
4. Check the fonts cover the needed characters (`tools/gen_fonts.sh`; currently Latin-1 plus
   a few typographic signs).
5. Extend `check_i18n.py` to check the new table.
