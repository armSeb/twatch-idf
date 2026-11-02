/*
 * crashlog.h — résumé du dernier plantage (coredump en flash), lisible sur
 * la montre (À propos) sans ordinateur : tâche, cause, PC et pile d'appels,
 * à décoder avec l'ELF de la version (xtensa-esp32-elf-addr2line).
 */
#pragma once
#include <stddef.h>

void        crashlog_init(void);        /* au démarrage */
const char *crashlog_text(void);        /* "" si aucun plantage enregistré */

/* Fil d'Ariane : dernières actions d'interface, gardé en RAM RTC à travers
 * un plantage et affiché avec son résumé. tag = 1 lettre, a/b = petits nombres. */
void crashlog_mark(char tag, int a, int b);

/* Vérifie l'intégrité du tas ; en cas de corruption, note le fil d'Ariane et
 * plante tout de suite (au plus près de la cause). where = 1 lettre. */
void crashlog_heap_check(char where);
