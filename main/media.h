/*
 * media.h — lecture en cours sur l'iPhone via AMS (Apple Media Service).
 *
 * Fonctionne avec toute app audio iOS (Spotify, Musique, Deezer, podcasts…) :
 * titre, artiste, album, durée, position, état, volume, et commandes
 * lecture/pause/suivant/précédent/volume. AMS ne transmet pas la pochette.
 *
 * Le client tourne dans la tâche hôte NimBLE (ancs.c) ; ces fonctions sont
 * thread-safe et servent à l'UI (copie par valeur, file de commandes).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* RemoteCommandID (spécification AMS) */
enum {
    MEDIA_CMD_PLAY = 0, MEDIA_CMD_PAUSE = 1, MEDIA_CMD_TOGGLE = 2,
    MEDIA_CMD_NEXT = 3, MEDIA_CMD_PREV = 4,
    MEDIA_CMD_VOL_UP = 5, MEDIA_CMD_VOL_DOWN = 6,
};

/* PlaybackState */
enum { MEDIA_PAUSED = 0, MEDIA_PLAYING = 1, MEDIA_REWINDING = 2, MEDIA_FFWD = 3 };

#define MEDIA_TXT_MAX 96

typedef struct {
    bool     available;            /* AMS abonné sur ce lien */
    char     player[40];           /* nom de l'app (vide = aucun lecteur) */
    char     title[MEDIA_TXT_MAX];
    char     artist[MEDIA_TXT_MAX];
    char     album[MEDIA_TXT_MAX];
    int      state;                /* MEDIA_PAUSED… */
    float    rate;
    float    elapsed_s;            /* position au moment elapsed_at_us */
    int64_t  elapsed_at_us;        /* esp_timer_get_time() de la mesure */
    float    duration_s;           /* 0 = inconnue */
    float    volume;               /* 0..1, <0 = inconnu */
    uint16_t cmds;                 /* bit n = RemoteCommandID n disponible */
    uint32_t seq;                  /* incrémenté à chaque changement */
} media_info_t;

void media_get(media_info_t *out);          /* copie cohérente */
void media_command(uint8_t cmd);            /* MEDIA_CMD_*, envoyé dès que le lien est libre */
float media_position_s(const media_info_t *m); /* position extrapolée maintenant */
