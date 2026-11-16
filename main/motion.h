/*
 * motion.h — accéléromètre BMA423 : compteur de pas et « lever le poignet »
 * pour allumer l'écran, deux fonctions activables séparément (réglages).
 *
 * Les deux tournent dans le moteur de fonctions du capteur (microcode Bosch
 * chargé au démarrage) : l'ESP32 ne fait rien tant qu'on ne regarde pas les
 * pas, et le geste arrive par la ligne INT1 (GPIO39, source de réveil).
 * Aucune des deux activée : capteur en suspend (~2,5 µA), comme avant.
 *
 * Pas : total du jour = base persistée + compteur du capteur ; à minuit
 * (heure locale) le jour passe dans l'historique (7 jours) et le compteur
 * repart de zéro. Sauvegarde en NVS toutes les 15 min si ça a bougé, au
 * changement de jour, à la désactivation et avant un redémarrage.
 *
 * Tout est appelé depuis la tâche app (I2C partagé, pas de verrou ici),
 * sauf motion_init (power_init, au démarrage).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MOTION_HIST_DAYS 7

esp_err_t motion_init(void);          /* power_init : suspend ou fonctions selon les réglages */
void      motion_apply(void);         /* après un changement des réglages */
bool      motion_on_irq(void);        /* EV_MOTION : true = geste « lever le poignet » */
/* Après un geste : confirme d'après la position de la montre selon la
 * sensibilité (0 basse … 2 haute = geste Bosch seul). Bloque 0 à ~0,5 s. */
bool      motion_wrist_confirm(int sens);
void      motion_tick(void);          /* périodique : minuit, sauvegarde */

bool      motion_ok(void);            /* capteur et microcode prêts */
bool      motion_steps_on(void);
uint32_t  motion_steps_today(void);
/* h[0] = hier … h[6] = il y a 7 jours (0 si inconnu) */
void      motion_history(uint32_t h[MOTION_HIST_DAYS]);
