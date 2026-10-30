# Écrire une app, traduire

Français · [English](../en/apps-and-i18n.md)

## Apps

Une app est un seul fichier, `main/app_xxx.c`, qui définit un `const app_t`, plus une ligne
dans `main/apps.c`. Elle apparaît alors dans le lanceur.

```c
typedef struct app_s {
    const char *name;                 /* libellé du lanceur (clé française, voir i18n) */
    const char *icon;                 /* symbole de font_title_20 (ui_kit.h) */
    uint32_t    color;                /* couleur du disque d'icône, 0xRRGGBB */
    void (*create)(lv_obj_t *scr);    /* construire la vue dans scr */
    void (*destroy)(void);            /* oublier les pointeurs vers la vue (optionnel) */
    void (*on_show)(void);            /* optionnel */
    void (*on_hide)(void);            /* optionnel */
    bool (*on_event)(int code);       /* app_post_event() ; true = ouvrir l'app, allumer l'écran */
    uint32_t screen_timeout_ms;       /* 0 = réglage utilisateur */
    bool (*busy)(void);               /* true = garder l'écran sur l'app à l'extinction et au réveil */
} app_t;
```

### Règles

- **La vue est détruite à l'extinction de l'écran** (pour la RAM) et reconstruite à la
  prochaine ouverture. L'état durable vit dans des variables `static` — le *modèle* —, jamais
  dans les objets LVGL. Remettre les pointeurs de la vue à `NULL` dans `destroy()`.
- Le système fournit un écran vide où le geste → ramène déjà à l'écran d'où l'app a été
  ouverte. `ui_header(scr, titre)` ajoute l'en-tête standard « ‹ Titre » (40 px).
- Tout tourne dans la tâche app (seule propriétaire de LVGL). Depuis un autre contexte
  (callback d'`esp_timer`, tâche), appeler `app_post_event(&app_xxx, code)` : le système le
  remet à `on_event(code)` dans la tâche app, même app fermée. Renvoyer `true` pour que
  l'app soit ouverte et l'écran allumé (alarme).
- Rafraîchissement périodique : créer un `lv_timer` dans `create()`, le mettre en pause dans
  `on_hide()`, le relancer dans `on_show()`, le supprimer dans `destroy()`.
- `busy()` qui renvoie vrai (chrono lancé…) fait s'éteindre l'écran *sur* l'app et le réveil
  y revenir au lieu du cadran.

### Briques (`ui_kit.h`)

Palette `C_BG`, `C_FG`, `C_DIM`, `C_OFF`, `C_CARD`, `C_ACC`, `C_OK`, `C_WARN`, `C_BAD` ;
icônes `SYM_*` ; `ui_label`, `ui_button`, `ui_container`, `ui_segmented`, `ui_header`,
`ui_toast`, `ui_bubble` (laisse les gestes atteindre l'écran à travers un objet enfant).

### Exemple minimal

```c
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"

static int s_count;                         /* modèle : survit à la vue */
static lv_obj_t *v_lbl;                     /* vue */

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

Puis dans `main/apps.c` : `extern const app_t app_counter;` et `&app_counter,` dans
`g_apps[]`, et la traduction anglaise de `"Compteur"` dans `main/i18n.c`.

Apps livrées à lire : `app_chrono.c` (modèle/vue, `busy`), `app_timer.c` (alarme
`esp_timer` écran éteint, `on_event`), `app_lamp.c` (luminosité à l'ouverture et à la
fermeture), `app_tvoff.c` (travail dans une tâche séparée, progression).

## Traduire

Le mécanisme suit gettext, sans outil : **la chaîne française sert de clé**.

- Dans le code : `_("Réglages")` renvoie la chaîne dans la langue courante.
- Dans les tableaux statiques, marquer les chaînes avec `N_("...")` (sans effet) et traduire
  à l'affichage : `_(table[i])`.
- `main/i18n.c` contient la table français → anglais. Une entrée manquante retombe sur le
  français.
- Les formats printf gardent les mêmes spécificateurs dans le même ordre dans les deux
  langues (`"Tour %d   %s"` → `"Lap %d   %s"`).
- `host_tests/check_i18n.py` (lancé par `host_tests/run.sh` et la CI) échoue sur toute chaîne
  `_()`/`N_()` sans traduction ou aux formats différents, et liste les entrées inutilisées.
- Changer de langue (*Réglages › Affichage › Langue*) reconstruit tous les écrans sur place.

### Ajouter une langue

1. Ajouter `LANG_XX` à l'énumération de `main/i18n.h`.
2. Ajouter une seconde table dans `main/i18n.c` et la choisir dans `i18n_tr()`.
3. Ajouter la langue au sélecteur de `build_menu()` (`main/ui.c`) et accepter la valeur
   dans `settings.c`.
4. Vérifier que les polices couvrent les caractères nécessaires (`tools/gen_fonts.sh` ;
   actuellement Latin-1 et quelques signes typographiques).
5. Étendre `check_i18n.py` à la nouvelle table.
