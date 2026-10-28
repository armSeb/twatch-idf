#include "ota.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include "ble.h"
#include "power.h"

static const char *TAG = "ota";

#define OTA_SSID          "T-Watch-MAJ"
#define VALIDATE_AFTER_US (30LL * 1000000)
#define IDLE_TIMEOUT_US   (5LL * 60 * 1000000)
#define MIN_BATT_PCT      30

static bool s_pending_verify;
static bool s_rolled_back;
static bool s_active;
static bool s_ble_stopped;     /* Bluetooth arrêté par nous : l'abandon redémarre */
static char s_pass[9];
static char s_new_ver[32];
static httpd_handle_t s_httpd;
static volatile int64_t s_last_activity_us;
static esp_timer_handle_t s_idle_timer;

/* ------------------------------------------------------------------ */
/* Validation de l'image au démarrage (retour arrière automatique)      */
/* ------------------------------------------------------------------ */
void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "nouveau firmware en essai (%s) : validé après 30 s sans plantage", run->label);
    }
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) {
        s_rolled_back = true;
        ESP_LOGW(TAG, "le firmware de %s avait échoué : retour à %s", bad->label, run ? run->label : "?");
    }
    ESP_LOGI(TAG, "démarré sur %s, version %s", run ? run->label : "?", esp_app_get_description()->version);
}

void ota_tick(void)
{
    if (s_pending_verify && esp_timer_get_time() > VALIDATE_AFTER_US) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "firmware validé");
        }
        s_pending_verify = false;
    }
}

bool ota_rolled_back(void) { return s_rolled_back; }

/* ------------------------------------------------------------------ */
/* Page web (en flash)                                                  */
/* ------------------------------------------------------------------ */
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=fr><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>T-Watch : mise à jour</title><style>"
    "body{font:16px -apple-system,system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:24px;max-width:480px}"
    "h1{font-size:22px;margin:0 0 4px}p{color:#9aa}input{margin:16px 0;color:#eee}"
    "button{font-size:17px;padding:12px 20px;border:0;border-radius:10px;background:#3b82f6;color:#fff;width:100%}"
    "button:disabled{background:#334}progress{width:100%;height:14px;margin-top:16px}"
    "#m{margin-top:12px;min-height:1.4em}.ok{color:#4ade80}.ko{color:#f87171}"
    "</style></head><body><h1>T-Watch 2020 · mise à jour</h1><p>Version installée : <b>";
static const char PAGE_TAIL[] =
    "</b></p><p>Choisir le fichier <b>twatch2020v1-app.bin</b>. L'ancien firmware reste en place "
    "jusqu'à la fin : une coupure ne casse rien.</p>"
    "<input type=file id=f accept='.bin,application/octet-stream'>"
    "<button id=b onclick=go()>Envoyer</button><progress id=p max=100 value=0 hidden></progress>"
    "<div id=m></div><script>"
    "function go(){var f=document.getElementById('f').files[0],m=document.getElementById('m'),"
    "p=document.getElementById('p'),b=document.getElementById('b');"
    "if(!f){m.textContent='Choisis d\\u2019abord le fichier.';return;}"
    "b.disabled=true;p.hidden=false;m.className='';m.textContent='Envoi\\u2026';"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total;};"
    "x.onload=function(){if(x.status==200){p.value=100;m.className='ok';"
    "m.textContent='Termin\\u00e9 : la montre red\\u00e9marre ('+x.responseText+').';}"
    "else{m.className='ko';m.textContent='\\u00c9chec : '+x.responseText;b.disabled=false;}};"
    "x.onerror=function(){m.className='ko';m.textContent='Connexion perdue.';b.disabled=false;};"
    "x.send(f);}</script></body></html>";

static esp_err_t page_get(httpd_req_t *r)
{
    s_last_activity_us = esp_timer_get_time();
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(r, esp_app_get_description()->version, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(r, PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static void post_ev(int32_t code)
{
    app_post_simple(EV_OTA, code);
}

static esp_err_t fail(httpd_req_t *r, esp_ota_handle_t h, int32_t code, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGW(TAG, "échec : %s", msg);
    post_ev(code);
    httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, msg);
}

/* Réception en flux : 4 Ko à la fois, écrits dans l'emplacement libre */
static esp_err_t update_post(httpd_req_t *r)
{
    static char buf[4096];
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    s_last_activity_us = esp_timer_get_time();
    if (!dst) return fail(r, 0, OTA_EV_ERR_WRITE, "pas d'emplacement OTA (table de partitions ?)");
    if (r->content_len <= 0 || r->content_len > dst->size) {
        return fail(r, 0, OTA_EV_ERR_SIZE, "taille de fichier invalide");
    }
    ESP_LOGI(TAG, "réception de %u octets vers %s", (unsigned)r->content_len, dst->label);

    esp_ota_handle_t h = 0;
    size_t left = r->content_len, done = 0;
    int last_pct = -1, timeouts = 0;
    bool checked = false;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) return fail(r, h, OTA_EV_ERR_NET, "transfert interrompu");
        timeouts = 0;
        s_last_activity_us = esp_timer_get_time();
        if (!checked) {
            /* en-tête ESP32 + descripteur d'application : bien un firmware T-Watch ? */
            const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if ((size_t)n < off + sizeof(esp_app_desc_t) || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                return fail(r, 0, OTA_EV_ERR_IMAGE, "ce fichier n'est pas un firmware ESP32");
            }
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + off);
            if (d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name))) {
                return fail(r, 0, OTA_EV_ERR_IMAGE, "ce n'est pas un firmware T-Watch (twatch)");
            }
            strlcpy(s_new_ver, d->version, sizeof(s_new_ver));
            if (esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                return fail(r, 0, OTA_EV_ERR_WRITE, "impossible de préparer la flash");
            }
            checked = true;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) return fail(r, h, OTA_EV_ERR_WRITE, "erreur d'écriture flash");
        left -= n;
        done += n;
        int pct = (int)(done * 100 / r->content_len);
        if (pct != last_pct) { last_pct = pct; post_ev(pct); }
    }
    esp_err_t e = esp_ota_end(h);          /* vérifie l'image complète (SHA-256) */
    if (e != ESP_OK) {
        return fail(r, 0, OTA_EV_ERR_IMAGE, e == ESP_ERR_OTA_VALIDATE_FAILED ? "image corrompue" : "image refusée");
    }
    if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        return fail(r, 0, OTA_EV_ERR_WRITE, "impossible d'activer la nouvelle image");
    }
    ESP_LOGI(TAG, "image %s installée sur %s", s_new_ver, dst->label);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, s_new_ver);
    post_ev(OTA_EV_DONE);
    return ESP_OK;
}

/* toute autre adresse renvoie la page (pratique si l'iPhone teste le réseau) */
static esp_err_t any_get(httpd_req_t *r) { return page_get(r); }

/* Mot de passe du point d'accès : tiré au sort une seule fois puis gardé
 * en NVS, pour que l'iPhone se reconnecte tout seul aux mises à jour
 * suivantes (conservé par les OTA et la mise à jour USB sans effacement). */
static void load_or_create_password(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_pass);
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
        return;
    }
    if (nvs_get_str(h, "otapw", s_pass, &len) != ESP_OK || strlen(s_pass) != 8) {
        snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
        nvs_set_str(h, "otapw", s_pass);
        nvs_commit(h);
        ESP_LOGI(TAG, "mot de passe du point d'accès créé");
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Point d'accès                                                        */
/* ------------------------------------------------------------------ */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_last_activity_us = esp_timer_get_time();
        post_ev(OTA_EV_CLIENT);
    }
}

static void idle_cb(void *arg)
{
    if (s_active && esp_timer_get_time() - s_last_activity_us > IDLE_TIMEOUT_US) post_ev(OTA_EV_TIMEOUT);
}

const char *ota_start(void)
{
    if (s_active) return NULL;
    if (!power_vbus() && power_percent() < MIN_BATT_PCT) return "Batterie trop faible (30 % mini) : brancher l'USB";
    if (!esp_ota_get_next_update_partition(NULL)) return "Table de partitions sans OTA : un flash USB est nécessaire";

    ble_stop();                                  /* libère la radio et la RAM du Bluetooth */
    s_ble_stopped = true;
    power_hold_nosleep(true);

    static bool net_inited;
    if (!net_inited) {
        esp_netif_init();
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return "Erreur réseau (boucle d'événements)";
        esp_netif_create_default_wifi_ap();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event, NULL);
        net_inited = true;
    }
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wc) != ESP_OK) return "Erreur Wi-Fi (mémoire ?)";
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    load_or_create_password();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, OTA_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(OTA_SSID);
    strlcpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) return "Le point d'accès n'a pas démarré";

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.max_uri_handlers = 4;
    hc.recv_wait_timeout = 10;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) return "Le serveur web n'a pas démarré";
    static const httpd_uri_t u_upd = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
    static const httpd_uri_t u_any = { .uri = "/*", .method = HTTP_GET, .handler = any_get };
    httpd_register_uri_handler(s_httpd, &u_upd);
    httpd_register_uri_handler(s_httpd, &u_any);

    s_last_activity_us = esp_timer_get_time();
    if (!s_idle_timer) {
        const esp_timer_create_args_t ta = { .callback = idle_cb, .name = "ota_idle" };
        esp_timer_create(&ta, &s_idle_timer);
    }
    esp_timer_start_periodic(s_idle_timer, 10 * 1000000);
    s_new_ver[0] = 0;
    s_active = true;
    ESP_LOGI(TAG, "point d'accès %s prêt, http://192.168.4.1", OTA_SSID);
    return NULL;
}

void ota_reboot_now(void)
{
    if (s_httpd) httpd_stop(s_httpd);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* L'abandon redémarre : retour garanti à un état propre (Bluetooth compris) */
void ota_cancel(void)
{
    if (!s_active && !s_ble_stopped) return;
    ESP_LOGI(TAG, "mise à jour abandonnée");
    ota_reboot_now();
}

bool ota_active(void) { return s_active; }
const char *ota_ssid(void) { return OTA_SSID; }
const char *ota_password(void) { return s_pass; }
const char *ota_new_version(void) { return s_new_ver; }
