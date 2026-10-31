/*
 * notif_store.h — stockage des notifications (propriété exclusive de la
 * tâche app). Tri par date iOS, déduplication par contenu (FNV-1a).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ancs_types.h"

#define NS_MAX 20
#define NS_DEDUP_TTL_S 300

typedef struct {
    ancs_notif_t n;
    uint32_t hash;
    int64_t  rx_us;
    bool     uid_valid;     /* faux après déconnexion : actions iOS impossibles */
} notif_entry_t;

/* nettoie la notif (UTF-8 valide, glyphes absents des polices retirés) puis
 * l'insère ou fusionne. *alert = vrai si c'est une nouveauté à signaler. */
notif_entry_t *ns_add(ancs_notif_t *n, bool *alert);
bool           ns_remove_uid(uint32_t uid);
void           ns_remove_entry(const notif_entry_t *e);
notif_entry_t *ns_find_uid(uint32_t uid);
notif_entry_t *ns_find_hash(uint32_t hash);  /* identifiant stable côté UI */
void           ns_invalidate_uids(void);
void           ns_set_alert_since(const char *date); /* NULL = replay toujours silencieux */
int            ns_prune_invalid(void);              /* retire les entrées non rejouées */
void           ns_clear(void);
int            ns_count(void);
notif_entry_t *ns_get(int idx);     /* 0 = plus récente */

/* utilitaire exposé pour l'UI */
void text_sanitize(char *s);
