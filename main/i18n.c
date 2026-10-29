/*
 * i18n.c — table français → anglais (voir i18n.h).
 * Ajouter une chaîne : l'écrire en français dans le code avec _(), puis ici ;
 * host_tests/check_i18n.py signale les manques et les formats incohérents.
 */
#include <string.h>
#include "i18n.h"

typedef struct { const char *fr, *en; } tr_t;

static const tr_t EN[] = {
    /* jours et mois (cadrans) */
    { "dim.", "Sun" }, { "lun.", "Mon" }, { "mar.", "Tue" }, { "mer.", "Wed" },
    { "jeu.", "Thu" }, { "ven.", "Fri" }, { "sam.", "Sat" },
    { "janv.", "Jan" }, { "févr.", "Feb" }, { "mars", "Mar" }, { "avr.", "Apr" },
    { "mai", "May" }, { "juin", "Jun" }, { "juil.", "Jul" }, { "août", "Aug" },
    { "sept.", "Sep" }, { "oct.", "Oct" }, { "nov.", "Nov" }, { "déc.", "Dec" },
    { "DIM", "SUN" }, { "LUN", "MON" }, { "MAR", "TUE" }, { "MER", "WED" },
    { "JEU", "THU" }, { "VEN", "FRI" }, { "SAM", "SAT" },
    { "RÉSERVE", "RESERVE" }, { "CHARGE", "CHARGING" },

    /* notifications, appel */
    { "Effacer", "Clear" },
    { "Appel entrant", "Incoming call" },
    { "Inconnu", "Unknown" },
    { "Aucune notification", "No notifications" },
    { "Tout effacer", "Clear all" },
    { "Notifications", "Notifications" },

    /* réglages */
    { "Réglages", "Settings" },
    { "Affichage", "Display" },
    { "Alertes et réveil", "Alerts & wake" },
    { "Alertes", "Alerts" },
    { "Bluetooth", "Bluetooth" },
    { "Énergie", "Battery" },
    { "À propos", "About" },
    { "Mise à jour", "Update" },
    { "Rétro", "Retro" }, { "Pub", "Pub" }, { "Digital", "Digital" },
    { "CADRAN", "WATCH FACE" },
    { "LUMINOSITÉ", "BRIGHTNESS" },
    { "EXTINCTION DE L'ÉCRAN", "SCREEN TIMEOUT" },
    { "LANGUE", "LANGUAGE" },
    { "Vibration", "Vibration" },
    { "Notifications et appels", "Notifications and calls" },
    { "Allumer l'écran", "Wake the screen" },
    { "Sinon : vibreur seul", "Otherwise: vibrate only" },
    { "Double tap", "Double tap" },
    { "2 tapes sur l'écran éteint", "Tap the dark screen twice" },
    { "Coupé = mode avion", "Off = airplane mode" },
    { "Appairer un iPhone", "Pair an iPhone" },
    { "Oublier les appareils", "Forget devices" },
    { "Oublier tous les\nappareils appairés ?", "Forget all\npaired devices?" },
    { "Il faudra réappairer l'iPhone.", "You will need to pair the iPhone again." },
    { "Annuler", "Cancel" },
    { "Oublier", "Forget" },
    { "Bluetooth activé", "Bluetooth on" },
    { "Mode avion", "Airplane mode" },
    { "Activez d'abord le Bluetooth", "Turn Bluetooth on first" },
    { "iPhone : Réglages > Bluetooth\n> T-Watch, puis « Jumeler »", "iPhone: Settings > Bluetooth\n> T-Watch, then \"Pair\"" },
    { "Annuler d'abord la mise à jour", "Cancel the update first" },
    { "Appareils oubliés", "Devices forgotten" },
    { "Coupé (mode avion)", "Off (airplane mode)" },
    { "Connecté à l'iPhone", "Connected to iPhone" },
    { "En attente de l'iPhone", "Waiting for iPhone" },
    { "Connexion en cours…", "Connecting…" },
    { "Aucun appareil appairé", "No paired device" },
    { "1 appareil appairé", "1 paired device" },
    { "%d appareils appairés", "%d paired devices" },
    { "Cadran %s · veille %u s", "%s face · sleep %u s" },
    { "Vibreur", "Vibrate" },
    { "Sans vibreur", "No vibration" },
    { " · discret", " · silent wake" },
    { " · double tap", " · double tap" },
    { "Connecté", "Connected" },
    { "Connexion…", "Connecting…" },
    { "%d %% · moyenne %.1f mA", "%d %% · average %.1f mA" },
    { " · en charge", " · charging" },
    { "Firmware %s", "Firmware %s" },
    { "Par Wi-Fi, sans câble", "Over Wi-Fi, no cable" },

    /* mise à jour */
    { "Version installée : %s\n\nLa montre crée un réseau Wi-Fi ; le Bluetooth est coupé "
      "le temps de la mise à jour.%s",
      "Installed version: %s\n\nThe watch creates a Wi-Fi network; Bluetooth is off "
      "during the update.%s" },
    { "\n\nLa dernière mise à jour avait échoué : ancien firmware restauré.",
      "\n\nThe last update failed: previous firmware restored." },
    { "Démarrer", "Start" },
    { "Démarrage du Wi-Fi…", "Starting Wi-Fi…" },
    { "Réception… %d %%", "Receiving… %d %%" },
    { "Appareil connecté : ouvrir http://192.168.4.1", "Device connected: open http://192.168.4.1" },
    { "Version %s installée\nRedémarrage…", "Version %s installed\nRestarting…" },
    { "Fichier refusé (pas un firmware T-Watch ?)", "File rejected (not a T-Watch firmware?)" },
    { "Fichier trop gros", "File too large" },
    { "Transfert interrompu : réessayer", "Transfer interrupted: try again" },
    { "Erreur d'écriture", "Write error" },
    { "Batterie trop faible (30 % mini) : brancher l'USB", "Battery too low (30 % min): plug in USB" },
    { "Table de partitions sans OTA : un flash USB est nécessaire", "No OTA partitions: a USB flash is required" },
    { "Erreur réseau (boucle d'événements)", "Network error (event loop)" },
    { "Erreur Wi-Fi (mémoire ?)", "Wi-Fi error (memory?)" },
    { "Le point d'accès n'a pas démarré", "The access point did not start" },
    { "Le serveur web n'a pas démarré", "The web server did not start" },
    { "Wi-Fi « %s », mot de passe :", "Wi-Fi \"%s\", password:" },
    { "Puis ouvrir\nhttp://192.168.4.1", "Then open\nhttp://192.168.4.1" },

    /* énergie */
    { "Batterie  %d %%  ·  %.3f V", "Battery  %d %%  ·  %.3f V" },
    { "Charge  +%.0f mA", "Charging  +%.0f mA" },
    { "Secteur · charge terminée", "USB · fully charged" },
    { "Décharge  %.1f mA", "Draw  %.1f mA" },
    { "Chargé  %.0f mAh en %luh%02lu", "Charged  %.0f mAh in %luh%02lu" },
    { "Conso  %.2f mAh en %luh%02lu", "Used  %.2f mAh in %luh%02lu" },
    { "Débrancher pour mesurer", "Unplug to measure" },
    { "Moy. %.1f mA, reste ~%.0f h", "Avg %.1f mA, ~%.0f h left" },
    { "Moyenne : mesure en cours…", "Average: measuring…" },
    { "coupé", "off" }, { "publicité", "advertising" }, { "connecté", "connected" },
    { "chiffré", "encrypted" }, { "prêt", "ready" },
    { "Veille : sur batterie", "Sleep: on battery only" },
    { "Veille %.1f mA · écran %.0f%%", "Sleep %.1f mA · screen %.0f%%" },
    { "Écran allumé %.0f %% du temps", "Screen on %.0f %% of the time" },
    { "Jauge : capacité ~%u mAh (%d mes.)", "Gauge: capacity ~%u mAh (%d meas.)" },
    { "Jauge : calibrage, %u mAh nominal", "Gauge: calibrating, %u mAh nominal" },
    { "Jauge : estimée (pas encore calibrée)", "Gauge: estimated (not calibrated yet)" },
    { "rechargée", "recharged" }, { "coupure", "power cut" }, { "redémarrage", "restart" },
    { "Préc. %.0f mAh en %luh%02lu, %s", "Prev. %.0f mAh in %luh%02lu, %s" },

    /* appairage, à propos */
    { "Appairage", "Pairing" },
    { "Saisir ce code sur l'iPhone", "Enter this code on the iPhone" },
    { "Appairage réussi", "Paired" },
    { "Échec de l'appairage", "Pairing failed" },
    { "Firmware %s\nESP-IDF %s · LVGL %d.%d.%d\n"
      "BLE %02X:%02X:%02X:%02X:%02X:%02X\nAppareils appairés : %d\n"
      "En marche depuis %luj %02luh%02lu\nTas libre %u Ko (min %u Ko)\nPlus grand bloc %u Ko\nReset : %d\n"
      "Réveils écran éteint :\n tactile %lu · notif %lu\n média %lu · AXP %lu · BLE %lu",
      "Firmware %s\nESP-IDF %s · LVGL %d.%d.%d\n"
      "BLE %02X:%02X:%02X:%02X:%02X:%02X\nPaired devices: %d\n"
      "Up for %lud %02luh%02lu\nFree heap %u KB (min %u KB)\nLargest block %u KB\nReset: %d\n"
      "Wake-ups, screen off:\n touch %lu · notif %lu\n media %lu · AXP %lu · BLE %lu" },
    { "\n\nJournal BLE (pub. %s) :\n", "\n\nBLE log (adv. %s):\n" },

    /* lecture en cours */
    { "Aucune lecture en cours", "Nothing playing" },
    { "iPhone non connecté", "iPhone not connected" },
    { "Lecture", "Now playing" },

    /* lanceur, contrôles rapides, apps */
    { "Apps", "Apps" },
    { "iPhone connecté", "iPhone connected" },
    { "Bluetooth coupé", "Bluetooth off" },
    { "iPhone absent", "iPhone away" },
    { "Ne pas déranger", "Do not disturb" },
    { "Notifications actives", "Notifications on" },
    { "Lampe", "Torch" },
    { "Luminosité", "Brightness" },
    { "Chrono", "Stopwatch" },
    { "Tour %d   %s", "Lap %d   %s" },
    { "Tour", "Lap" },
    { "Zéro", "Reset" },
    { "Toucher : blanc / rouge", "Tap: white / red" },
    { "Minuteur", "Timer" },
    { "Terminé !", "Time's up!" },
    { "Sonne à %02d:%02d", "Rings at %02d:%02d" },
    { "En pause", "Paused" },
    { "Minuteur indisponible", "Timer unavailable" },
    { "Code %u / %u", "Code %u / %u" },
    { "Démarrage…", "Starting…" },
    { "Terminé : tous les codes envoyés", "Done: all codes sent" },
    { "Interrompu", "Stopped" },
    { "Erreur infrarouge", "Infrared error" },
    { "Viser la télé, puis appuyer", "Aim at the TV, then press" },
    { "Tous", "All" },
};

static int s_lang = LANG_FR;

void i18n_set(int lang) { s_lang = (lang >= 0 && lang < LANG_COUNT) ? lang : LANG_FR; }
int  i18n_get(void) { return s_lang; }

const char *i18n_tr(const char *fr)
{
    if (s_lang == LANG_FR || !fr || !fr[0]) return fr;
    for (size_t i = 0; i < sizeof(EN) / sizeof(EN[0]); i++)
        if (EN[i].fr[0] == fr[0] && strcmp(EN[i].fr, fr) == 0) return EN[i].en;
    return fr;
}
