/* Allocation de l'état du décodeur Helix, fournie par l'application
 * (main/radio.c : RAM interne rapide si possible, sinon PSRAM). */
#pragma once
#include <stddef.h>
void *helix_malloc(size_t n);
void  helix_free(void *p);
