/* puca_gfx.h — généré par tools/puca_art.py, ne pas éditer */
#pragma once
#include <stdint.h>

typedef struct { uint8_t w, h; const uint8_t *px; } puca_spr_t;

enum {
    PC_NONE = 0,
    PC_OUTLINE = 1,
    PC_FUR_DARK = 2,
    PC_FUR = 3,
    PC_FUR_HI = 4,
    PC_FUR_LIGHT = 5,
    PC_PINK = 6,
    PC_MOUTH = 7,
    PC_MOUTH_IN = 8,
    PC_EYE = 9,
    PC_EYE_LOW = 10,
    PC_PUPIL = 11,
    PC_WHITE = 12,
    PC_HORN = 13,
    PC_HORN_DK = 14,
    PC_EGG = 15,
    PC_EGG_DK = 16,
    PC_EGG_SPOT = 17,
    PC_EGG_SPOT2 = 18,
    PC_POTATO = 19,
    PC_POTATO_DK = 20,
    PC_CAKE = 21,
    PC_CAKE_HI = 22,
    PC_ICING = 23,
    PC_RED = 24,
    PC_RED_DK = 25,
    PC_ZZZ = 26,
    PC_WATER = 27,
    PC_WATER_DK = 28,
    PC_POTION = 29,
    PC_POTION_DK = 30,
    PC_CORK = 31,
    PC_STINK = 32,
    PC_POOP = 33,
    PC_POOP_DK = 34,
    PC_SPARK = 35,
    PC_CLOVER = 36,
    PC_CLOVER_DK = 37,
    PC_CLOUD = 38,
    PC_CLOUD_DK = 39,
    PC_BALL = 40,
    PC_SKY1 = 41,
    PC_SKY2 = 42,
    PC_SKY3 = 43,
    PC_HILL_FAR = 44,
    PC_HILL_FAR2 = 45,
    PC_HILL = 46,
    PC_HILL_HI = 47,
    PC_GRASS = 48,
    PC_DIRT = 49,
    PC_STONE = 50,
    PC_STONE_DK = 51,
    PC_MOON = 52,
    PC_STAR = 53,
    PC_SHROOM = 54,
    PC_SHROOM_W = 55,
    PC_COUNT = 56
};

extern const uint32_t PUCA_PAL[PC_COUNT];   /* 0xRRGGBB */
extern const uint8_t PUCA_GLOW[PC_COUNT];   /* 1 = garde son éclat dans le noir */

typedef struct { int8_t ex1, ey1, ex2, ey2, mx, my; uint8_t nframes; const puca_spr_t *frames[2]; } puca_body_t;

extern const puca_spr_t puca_eye_open;
extern const puca_spr_t puca_eye_blink;
extern const puca_spr_t puca_eye_happy;
extern const puca_spr_t puca_eye_closed;
extern const puca_spr_t puca_eye_sad;
extern const puca_spr_t puca_eye_sick;
extern const puca_spr_t puca_eye_wide;
extern const puca_spr_t puca_mouth_none;
extern const puca_spr_t puca_mouth_smile;
extern const puca_spr_t puca_mouth_open;
extern const puca_spr_t puca_mouth_frown;
extern const puca_spr_t puca_mouth_flat;
extern const puca_spr_t puca_mouth_chew;
extern const puca_body_t puca_body_baby;
extern const puca_body_t puca_body_teen;
extern const puca_body_t puca_body_hare;
extern const puca_body_t puca_body_horse;
extern const puca_body_t puca_body_goat;
extern const puca_spr_t puca_egg_0;
extern const puca_spr_t puca_egg_1;
extern const puca_spr_t puca_egg_2;
extern const puca_spr_t puca_it_potato;
extern const puca_spr_t puca_it_potato2;
extern const puca_spr_t puca_it_potato3;
extern const puca_spr_t puca_it_cake;
extern const puca_spr_t puca_it_cake2;
extern const puca_spr_t puca_it_cake3;
extern const puca_spr_t puca_it_heart;
extern const puca_spr_t puca_it_heart_s;
extern const puca_spr_t puca_it_z;
extern const puca_spr_t puca_it_spark;
extern const puca_spr_t puca_it_spark_s;
extern const puca_spr_t puca_it_poop;
extern const puca_spr_t puca_it_stink;
extern const puca_spr_t puca_it_potion;
extern const puca_spr_t puca_it_cloud;
extern const puca_spr_t puca_it_rain;
extern const puca_spr_t puca_it_dot;
extern const puca_spr_t puca_it_dot_off;
extern const puca_spr_t puca_it_dizzy;
extern const puca_spr_t puca_it_ball;
extern const puca_spr_t puca_it_clover;
extern const puca_spr_t puca_it_raincloud;
extern const puca_spr_t puca_it_bolt;
extern const puca_spr_t puca_it_b_feed;
extern const puca_spr_t puca_it_b_play;
extern const puca_spr_t puca_it_b_clean;
extern const puca_spr_t puca_it_b_heal;
extern const puca_spr_t puca_it_b_moon;
extern const puca_spr_t puca_it_b_sun;
extern const puca_spr_t puca_it_wave;
extern const puca_spr_t puca_it_shell_l;
extern const puca_spr_t puca_it_moon;
extern const puca_spr_t puca_it_stone;
extern const puca_spr_t puca_it_shroom;
extern const puca_spr_t puca_it_bush;
