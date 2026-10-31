/*
 * crashlog.h — résumé du dernier plantage (coredump en flash), lisible sur
 * la montre (À propos) sans ordinateur : tâche, cause, PC et pile d'appels,
 * à décoder avec l'ELF de la version (xtensa-esp32-elf-addr2line).
 */
#pragma once
#include <stddef.h>

void        crashlog_init(void);        /* au démarrage */
const char *crashlog_text(void);        /* "" si aucun plantage enregistré */
