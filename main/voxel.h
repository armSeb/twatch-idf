/*
 * voxel.h — paysage irlandais en « voxel space » pour la Démo et le jeu de vol.
 *
 * Le monde (512×512, se répète sans couture) est généré à l'ouverture, en
 * tâche de fond sur le cœur 0 : relief à plusieurs octaves, rivières, lacs,
 * plages, prés en patchwork bordés de haies, forêts, fermettes blanches,
 * roche et neige, puis éclairage du soleil couchant avec ombres portées.
 * Rendu colonne par colonne avec brume, reflets dans l'eau, nuages en
 * perspective, soleil et halo, roulis par colonne, tampon de profondeur.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "demo_gfx.h"

#define VOX_N     512
#define VOX_SEA   20                 /* niveau de l'eau */
#define VOX_SUN_A 0.3f               /* direction du soleil (rad) */

typedef struct {
    float x, y;                      /* position sur la carte */
    float h;                         /* altitude */
    float a;                         /* cap (rad) */
    float roll;                      /* inclinaison : décalage d'horizon par colonne */
    float pitch;                     /* lignes : + = horizon plus bas (nez levé) */
} vox_cam_t;

bool  vox_gen_start(uint32_t seed);  /* false : mémoire insuffisante */
int   vox_progress(void);            /* 0..100 ; 100 = prêt */
void  vox_free(void);                /* arrête la génération et libère */
float vox_ground(float x, float y);  /* hauteur du sol (eau comprise), interpolée */
/* un point de départ dans une vallée dégagée, et le cap le plus libre */
void  vox_find_valley(float *x, float *y, float *dir);

/* fy : échelle verticale (≈ 70 pour 120 lignes) ; depth (optionnel) : z par
 * pixel, 255 = ciel ; t : temps (s) pour les reflets et les nuages */
void  vox_render(gsurf_t *s, uint8_t *depth, const vox_cam_t *c, float fy, float t);
/* projection d'un point du monde, false s'il est derrière la caméra */
bool  vox_project(const gsurf_t *s, const vox_cam_t *c, float fy, float wx, float wy, float wh,
                  float *sx, float *sy, float *z);
