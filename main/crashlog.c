/* crashlog.c — voir crashlog.h */
#include <stdio.h>
#include <string.h>
#include "crashlog.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"

#define TAG "crash"

static char s_txt[320];

void crashlog_init(void)
{
    s_txt[0] = 0;
    if (esp_core_dump_image_check() != ESP_OK) return;
    static esp_core_dump_summary_t sm;           /* ~200 o : hors pile */
    if (esp_core_dump_get_summary(&sm) != ESP_OK) return;

    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    bool same = strncmp(sha, (const char *)sm.app_elf_sha256, 8) == 0;
    char why[80] = "";
    esp_core_dump_get_panic_reason(why, sizeof(why));

    int n = snprintf(s_txt, sizeof(s_txt), "%s (%s, ELF %.8s)\n%s\nPC %08lx cause %lu adr %08lx\nBT",
                     same ? "Dernier plantage" : "Ancien plantage", sm.exc_task,
                     (const char *)sm.app_elf_sha256, why[0] ? why : "?",
                     (unsigned long)sm.exc_pc, (unsigned long)sm.ex_info.exc_cause,
                     (unsigned long)sm.ex_info.exc_vaddr);
    for (uint32_t i = 0; i < sm.exc_bt_info.depth && i < 8 && n > 0 && n < (int)sizeof(s_txt); i++)
        n += snprintf(s_txt + n, sizeof(s_txt) - n, " %08lx", (unsigned long)sm.exc_bt_info.bt[i]);
    if (sm.exc_bt_info.corrupted && n > 0 && n < (int)sizeof(s_txt))
        snprintf(s_txt + n, sizeof(s_txt) - n, " (pile corrompue)");
    ESP_LOGW(TAG, "%s", s_txt);
}

const char *crashlog_text(void) { return s_txt; }
