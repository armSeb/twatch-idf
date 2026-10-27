/*
 * ancs_types.h — types ANCS sans dépendance NimBLE (testables sur PC).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Catégories ANCS */
enum {
    ANCS_CAT_OTHER = 0, ANCS_CAT_INCOMING_CALL, ANCS_CAT_MISSED_CALL, ANCS_CAT_VOICEMAIL,
    ANCS_CAT_SOCIAL, ANCS_CAT_SCHEDULE, ANCS_CAT_EMAIL, ANCS_CAT_NEWS, ANCS_CAT_HEALTH,
    ANCS_CAT_FINANCE, ANCS_CAT_LOCATION, ANCS_CAT_ENTERTAINMENT,
};

/* EventFlags */
#define ANCS_FLAG_SILENT        (1 << 0)
#define ANCS_FLAG_IMPORTANT     (1 << 1)
#define ANCS_FLAG_PREEXISTING   (1 << 2)
#define ANCS_FLAG_POSITIVE      (1 << 3)
#define ANCS_FLAG_NEGATIVE      (1 << 4)

#define ANCS_APPID_MAX   48
#define ANCS_APPNAME_MAX 32
#define ANCS_TITLE_MAX   64
#define ANCS_MSG_MAX     240
#define ANCS_LABEL_MAX   20

typedef struct {
    uint32_t uid;
    uint8_t  category;
    uint8_t  flags;
    bool     modified;                   /* EventID Modified (et non Added) */
    char app_id[ANCS_APPID_MAX + 1];
    char app_name[ANCS_APPNAME_MAX + 1];
    char title[ANCS_TITLE_MAX + 1];
    char message[ANCS_MSG_MAX + 1];
    char date[16];                       /* yyyyMMdd'T'HHmmSS */
    char pos_label[ANCS_LABEL_MAX + 1];
    char neg_label[ANCS_LABEL_MAX + 1];
} ancs_notif_t;

/* Réassemblage des réponses Data Source.
 * retour : 1 = complet, 0 = incomplet (attendre la suite), -1 = incohérent */
int ancs_parse_notif_attrs(const uint8_t *buf, size_t len, uint32_t uid, int nattrs, ancs_notif_t *out);
int ancs_parse_app_attrs(const uint8_t *buf, size_t len, char *name, size_t name_sz);
