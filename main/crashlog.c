/* crashlog.c — voir crashlog.h */
#include <stdio.h>
#include <string.h>
#include "crashlog.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#define TAG "crash"

static char s_txt[520];

#define CRUMBS 20
#define CRUMB_MAGIC 0x5EB0C0DEu
typedef struct { uint32_t ms; char tag; int8_t a, b; } crumb_t;
static RTC_NOINIT_ATTR crumb_t s_crumb[CRUMBS];
static RTC_NOINIT_ATTR uint32_t s_crumb_head, s_crumb_magic;
static char s_prev[CRUMBS * 14];             /* fil d'Ariane d'avant le redémarrage */
static portMUX_TYPE s_cmux = portMUX_INITIALIZER_UNLOCKED;

void crashlog_mark(char tag, int a, int b)
{
    portENTER_CRITICAL(&s_cmux);
    if (s_crumb_magic != CRUMB_MAGIC || s_crumb_head >= CRUMBS) { memset(s_crumb, 0, sizeof(s_crumb)); s_crumb_head = 0; s_crumb_magic = CRUMB_MAGIC; }
    crumb_t *c = &s_crumb[s_crumb_head];
    c->ms = (uint32_t)(esp_timer_get_time() / 1000);
    c->tag = tag; c->a = (int8_t)a; c->b = (int8_t)b;
    s_crumb_head = (s_crumb_head + 1) % CRUMBS;
    portEXIT_CRITICAL(&s_cmux);
}

void crashlog_heap_check(char where)
{
    /* RAM interne seulement : objets LVGL, piles, NimBLE (la PSRAM, 4 Mo à
     * relire en entier avec l'empoisonnement complet, serait trop lente) */
    if (heap_caps_check_integrity(MALLOC_CAP_INTERNAL, false)) return;
    crashlog_mark('!', where, 0);
    abort();                                   /* « abort() » + fil d'Ariane */
}

/* « dt tag a,b » du plus ancien au plus récent ; dt en ms avant la dernière */
static void crumbs_format(void)
{
    s_prev[0] = 0;
    if (s_crumb_magic != CRUMB_MAGIC || s_crumb_head >= CRUMBS) return;
    uint32_t last = s_crumb[(s_crumb_head + CRUMBS - 1) % CRUMBS].ms;
    size_t n = 0;
    for (int i = 0; i < CRUMBS; i++) {
        const crumb_t *c = &s_crumb[(s_crumb_head + i) % CRUMBS];
        if (!c->tag) continue;
        uint32_t dt = last - c->ms;
        int w = dt < 100000 ? snprintf(s_prev + n, sizeof(s_prev) - n, " %c%d.%d-%lu", c->tag, c->a, c->b, (unsigned long)dt)
                            : snprintf(s_prev + n, sizeof(s_prev) - n, " %c%d.%d", c->tag, c->a, c->b);
        if (w < 0 || (size_t)w >= sizeof(s_prev) - n) break;
        n += (size_t)w;
    }
}

void crashlog_init(void)
{
    s_txt[0] = 0;
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) crumbs_format();
    s_crumb_magic = 0;                           /* nouveau fil pour cette session */
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
    /* assert/abort : les 3 premiers niveaux sont panic_abort, esp_system_abort
     * et __assert_func/abort, sans intérêt ; on montre plutôt les appelants */
    uint32_t i0 = (strncmp(why, "assert", 6) == 0 || strncmp(why, "abort", 5) == 0) &&
                  sm.exc_bt_info.depth > 6 ? 3 : 0;
    if (i0 && n > 0 && n < (int)sizeof(s_txt)) n += snprintf(s_txt + n, sizeof(s_txt) - n, " …");
    for (uint32_t i = i0; i < sm.exc_bt_info.depth && i < i0 + 12 && n > 0 && n < (int)sizeof(s_txt); i++)
        n += snprintf(s_txt + n, sizeof(s_txt) - n, " %08lx", (unsigned long)sm.exc_bt_info.bt[i]);
    if (sm.exc_bt_info.corrupted && n > 0 && n < (int)sizeof(s_txt))
        n += snprintf(s_txt + n, sizeof(s_txt) - n, " (pile corrompue)");
    if (same && s_prev[0] && n > 0 && n < (int)sizeof(s_txt))
        snprintf(s_txt + n, sizeof(s_txt) - n, "\nAvant :%s", s_prev);
    ESP_LOGW(TAG, "%s", s_txt);
}

const char *crashlog_text(void) { return s_txt; }
