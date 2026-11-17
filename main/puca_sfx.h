/* puca_sfx.h — petits airs « 8 bits » de l'app Púca (non bloquant) */
#pragma once

typedef enum {
    SFX_TAP, SFX_HATCH, SFX_EAT, SFX_TREAT, SFX_HAPPY, SFX_REFUSE, SFX_CLEAN, SFX_HEAL,
    SFX_EVOLVE, SFX_WIN, SFX_LOSE, SFX_RIGHT, SFX_WRONG, SFX_CLOVER, SFX_LIGHT,
    SFX_COUNT
} puca_sfx_t;

void puca_sfx(puca_sfx_t s);
