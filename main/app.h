/*
 * app.h — cadre d'applications de la montre.
 *
 * Une app = une fiche app_t enregistrée dans apps.c. Le système lui fournit
 * un écran vide (gestes de retour déjà gérés) quand on l'ouvre depuis le
 * lanceur, et le détruit quand on la quitte ou à l'ouverture d'une autre
 * app (écran éteint, elle reste construite et cachée) : l'état durable de l'app vit donc dans ses variables statiques
 * (modèle), jamais dans les objets LVGL (vue).
 *
 * Toutes les fonctions sont appelées depuis la tâche app (propriétaire de
 * LVGL). Une app qui doit réagir hors de l'écran (minuteur…) poste un
 * événement avec app_post_event(), depuis n'importe quel contexte.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct app_s {
    const char *name;                 /* libellé du lanceur */
    const char *icon;                 /* symbole (font_title_20) */
    uint32_t    color;                /* couleur de l'icône, 0xRRGGBB */
    void (*create)(lv_obj_t *scr);    /* construire la vue dans scr */
    void (*destroy)(void);            /* oublier les pointeurs vers la vue (optionnel) */
    void (*on_show)(void);            /* optionnel */
    void (*on_hide)(void);            /* optionnel */
    /* événement posté par app_post_event ; renvoie true pour ouvrir l'app
     * et allumer l'écran (alarme…) */
    bool (*on_event)(int code);
    uint32_t screen_timeout_ms;       /* 0 = réglage utilisateur */
    /* optionnel : true = l'app travaille (chrono lancé…). Toute app ouverte
     * est retrouvée au réveil ; écran éteint, on_hide() est appelé et
     * on_show() au réveil (couper le Wi-Fi, les timers d'affichage…) */
    bool (*busy)(void);
} app_t;

/* registre (apps.c) */
extern const app_t *const g_apps[];
extern const int g_app_count;

/* services offerts aux apps */
void app_post_event(const app_t *app, int code);  /* thread-safe, ISR exclue */
void app_open(const app_t *app);                  /* ouvrir (depuis la tâche app) */
void app_close(void);                             /* revenir à l'écran d'origine */
bool app_is_open(const app_t *app);
/* réglages de l'app saisis sur la page web de la montre (point d'accès Wi-Fi,
 * Bluetooth coupé) ; la session finit par un redémarrage qui rouvre l'app */
void app_open_settings_portal(const app_t *app);
/* dans on_hide : true si l'on quitte vraiment l'app écran allumé (pas une
 * extinction, ni une notification ou un appel ouverts par-dessus) */
bool app_leaving_for_good(void);
