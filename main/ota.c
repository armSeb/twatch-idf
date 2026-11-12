#include "i18n.h"
#include "ota.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include "ble.h"
#include "power.h"
#include "net.h"
#include "webrx_cfg.h"

static const char *TAG = "ota";

#define OTA_SSID          "T-Watch-OTA"
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
/* Retour à la version précédente (l'autre emplacement OTA)             */
/* ------------------------------------------------------------------ */
static const esp_partition_t *other_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *o = esp_ota_get_next_update_partition(NULL);
    return (o && run && o->address != run->address) ? o : NULL;
}

/* Version de l'autre emplacement si c'est un firmware T-Watch qui n'a pas
 * été marqué invalide ; NULL sinon (vide, effacé, transfert interrompu…). */
const char *ota_other_version(void)
{
    static char ver[32];
    const esp_partition_t *o = other_slot();
    if (!o) return NULL;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(o, &d) != ESP_OK) return NULL;
    if (strncmp(d.project_name, esp_app_get_description()->project_name, sizeof(d.project_name))) return NULL;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(o, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) return NULL;
    strlcpy(ver, d.version, sizeof(ver));
    return ver;
}

/* Vérifie l'image complète (somme + SHA-256), l'active et redémarre.
 * Ne revient qu'en cas d'échec (message à afficher). */
const char *ota_boot_other(void)
{
    const esp_partition_t *o = other_slot();
    if (!o || !ota_other_version()) return N_("Aucune version précédente utilisable");
    const esp_partition_pos_t pos = { .offset = o->address, .size = o->size };
    esp_image_metadata_t md;
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &md) != ESP_OK) return N_("Version précédente abîmée");
    if (esp_ota_set_boot_partition(o) != ESP_OK) return N_("Impossible d'activer la version précédente");
    ESP_LOGI(TAG, "retour à la version précédente (%s)", o->label);
    ota_reboot_now();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Page web (en flash)                                                  */
/* ------------------------------------------------------------------ */
/* Page bilingue : français si le navigateur est en français, anglais sinon
 * (bouton FR/EN). Le serveur ne renvoie que des codes d'erreur courts,
 * traduits ici. */
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>T-Watch</title><style>"
    "body{font:16px -apple-system,system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:24px;max-width:480px}"
    "h1{font-size:22px;margin:0 0 4px}p{color:#9aa}input{margin:16px 0;color:#eee}"
    "button{font-size:17px;padding:12px 20px;border:0;border-radius:10px;background:#3b82f6;color:#fff;width:100%}"
    "button:disabled{background:#334}progress{width:100%;height:14px;margin-top:16px}"
    "#m{margin-top:12px;min-height:1.4em}.ok{color:#4ade80}.ko{color:#f87171}"
    "#l{float:right;background:none;border:1px solid #445;color:#9aa;width:auto;padding:4px 10px;font-size:13px}"
    "</style></head><body><button id=l onclick=sw()></button><h1 id=t></h1><p><span id=v></span> <b>";
static const char PAGE_TAIL[] =
    "</b></p><p id=h></p>"
    "<input type=file id=f accept='.bin,application/octet-stream'>"
    "<button id=b onclick=go()></button><progress id=p max=100 value=0 hidden></progress>"
    "<div id=m></div><p><a id=w href=/webrx style='color:#9aa'></a></p><script>"
    "var T={fr:{w:'Réglages WebRX →',t:'T-Watch 2020 · mise à jour',v:'Version installée :',"
    "h:'Choisir le fichier <b>twatch2020v1-app.bin</b>. L’ancien firmware reste en place jusqu’à la fin : une coupure ne casse rien.',"
    "b:'Envoyer',nf:'Choisis d’abord le fichier.',s:'Envoi…',ok:'Terminé : la montre redémarre ',"
    "ko:'Échec : ',lost:'Connexion perdue.',"
    "no_slot:'pas d’emplacement de mise à jour (flash USB nécessaire)',bad_size:'taille de fichier invalide',"
    "net:'transfert interrompu, réessayer',not_esp32:'ce fichier n’est pas un firmware ESP32',"
    "not_twatch:'ce n’est pas un firmware T-Watch',flash_prep:'impossible de préparer la flash',"
    "flash_write:'erreur d’écriture flash',corrupt:'image corrompue',rejected:'image refusée',"
    "activate:'impossible d’activer la nouvelle image'},"
    "en:{w:'WebRX settings →',t:'T-Watch 2020 · update',v:'Installed version:',"
    "h:'Choose the <b>twatch2020v1-app.bin</b> file. The current firmware stays in place until the end: an interruption breaks nothing.',"
    "b:'Send',nf:'Choose the file first.',s:'Sending…',ok:'Done: the watch is restarting ',"
    "ko:'Failed: ',lost:'Connection lost.',"
    "no_slot:'no update slot (USB flash required)',bad_size:'invalid file size',"
    "net:'transfer interrupted, try again',not_esp32:'this file is not an ESP32 firmware',"
    "not_twatch:'this is not a T-Watch firmware',flash_prep:'could not prepare the flash',"
    "flash_write:'flash write error',corrupt:'corrupted image',rejected:'image rejected',"
    "activate:'could not activate the new image'}};"
    "var L=(navigator.language||'en').slice(0,2)=='fr'?'fr':'en';"
    "function $(i){return document.getElementById(i);}"
    "function tr(k){return T[L][k]||k;}"
    "function draw(){document.documentElement.lang=L;['t','v','b','w'].forEach(function(k){$(k).textContent=tr(k);});"
    "$('h').innerHTML=tr('h');$('l').textContent=L=='fr'?'English':'Français';}"
    "function sw(){L=L=='fr'?'en':'fr';draw();}"
    "function go(){var f=$('f').files[0],m=$('m'),p=$('p'),b=$('b');"
    "if(!f){m.textContent=tr('nf');return;}"
    "b.disabled=true;p.hidden=false;m.className='';m.textContent=tr('s');"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total;};"
    "x.onload=function(){if(x.status==200){p.value=100;m.className='ok';"
    "m.textContent=tr('ok')+'('+x.responseText+').';}"
    "else{m.className='ko';m.textContent=tr('ko')+tr(x.responseText);b.disabled=false;}};"
    "x.onerror=function(){m.className='ko';m.textContent=tr('lost');b.disabled=false;};"
    "x.send(f);}draw();</script></body></html>";

/* Réponse d'un bloc avec Content-Length et « Connection: close » : Safari iOS
 * garde sinon plusieurs connexions ouvertes et sature le serveur. */
static esp_err_t page_get(httpd_req_t *r)
{
    static char page[sizeof(PAGE_HEAD) + sizeof(PAGE_TAIL) + 40];
    s_last_activity_us = esp_timer_get_time();
    int n = snprintf(page, sizeof(page), "%s%s%s", PAGE_HEAD, esp_app_get_description()->version, PAGE_TAIL);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Connection", "close");
    return httpd_resp_send(r, page, n);
}

/* Sondes « y a-t-il Internet ? » d'iOS / Android : on répond « oui », sinon le
 * téléphone traite le réseau comme un portail captif ou le boude. */
static esp_err_t probe_apple(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Connection", "close");
    return httpd_resp_sendstr(r, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

static esp_err_t probe_204(httpd_req_t *r)
{
    httpd_resp_set_status(r, "204 No Content");
    httpd_resp_set_hdr(r, "Connection", "close");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t not_found(httpd_req_t *r)
{
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_hdr(r, "Connection", "close");
    return httpd_resp_send(r, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Mini serveur DNS : toute question de type A → 192.168.4.1.           */
/* Le téléphone résout ainsi captive.apple.com vers la montre, voit la   */
/* réponse « Success » et considère le réseau comme normal.             */
/* ------------------------------------------------------------------ */
static TaskHandle_t s_dns_task;

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { s_dns_task = NULL; vTaskDelete(NULL); return; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) { close(sock); s_dns_task = NULL; vTaskDelete(NULL); return; }
    static uint8_t b[512];
    while (s_active) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (b[2] & 0x80) || b[4] != 0 || b[5] != 1) continue;   /* une seule question */
        int q = 12;
        while (q < n && b[q] != 0) q += b[q] + 1;                          /* nom de domaine */
        q += 5;                                                              /* 0 final + type + classe */
        if (q > n || q + 16 > (int)sizeof(b)) continue;
        bool is_a = b[q - 4] == 0 && b[q - 3] == 1;
        b[2] = 0x81; b[3] = 0x80;                                            /* réponse, sans erreur */
        b[6] = 0; b[7] = is_a ? 1 : 0;                                       /* nb de réponses */
        b[8] = b[9] = b[10] = b[11] = 0;
        int len = q;
        if (is_a) {
            static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(b + q, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, b, len, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* transfert en cours (tâche httpd dans update_post) / activation de l'image */
static volatile bool s_receiving, s_finishing;

static void post_ev(int32_t code)
{
    app_post_simple(EV_OTA, code);
}

static esp_err_t fail(httpd_req_t *r, esp_ota_handle_t h, int32_t code, const char *msg)
{
    if (h) esp_ota_abort(h);
    s_receiving = false;
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
    if (!dst) return fail(r, 0, OTA_EV_ERR_WRITE, "no_slot");
    if (r->content_len <= 0 || r->content_len > dst->size) {
        return fail(r, 0, OTA_EV_ERR_SIZE, "bad_size");
    }
    ESP_LOGI(TAG, "réception de %u octets vers %s", (unsigned)r->content_len, dst->label);

    esp_ota_handle_t h = 0;
    size_t left = r->content_len, done = 0;
    s_receiving = true;                    /* remis à false par fail() ou en fin */
    int last_pct = -1, timeouts = 0;
    bool checked = false;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) return fail(r, h, OTA_EV_ERR_NET, "net");
        timeouts = 0;
        s_last_activity_us = esp_timer_get_time();
        if (!checked) {
            /* en-tête ESP32 + descripteur d'application : bien un firmware T-Watch ? */
            const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if ((size_t)n < off + sizeof(esp_app_desc_t) || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                return fail(r, 0, OTA_EV_ERR_IMAGE, "not_esp32");
            }
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + off);
            if (d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name))) {
                return fail(r, 0, OTA_EV_ERR_IMAGE, "not_twatch");
            }
            strlcpy(s_new_ver, d->version, sizeof(s_new_ver));
            if (esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                return fail(r, 0, OTA_EV_ERR_WRITE, "flash_prep");
            }
            checked = true;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) return fail(r, h, OTA_EV_ERR_WRITE, "flash_write");
        left -= n;
        done += n;
        int pct = (int)(done * 100 / r->content_len);
        if (pct != last_pct) { last_pct = pct; post_ev(pct); }
    }
    s_finishing = true;                    /* plus d'annulation : otadata va être écrit */
    s_receiving = false;
    esp_err_t e = esp_ota_end(h);          /* vérifie l'image complète (SHA-256) */
    if (e != ESP_OK) {
        s_finishing = false;
        return fail(r, 0, OTA_EV_ERR_IMAGE, e == ESP_ERR_OTA_VALIDATE_FAILED ? "corrupt" : "rejected");
    }
    if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        s_finishing = false;
        return fail(r, 0, OTA_EV_ERR_WRITE, "activate");
    }
    ESP_LOGI(TAG, "image %s installée sur %s", s_new_ver, dst->label);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, s_new_ver);
    post_ev(OTA_EV_DONE);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Page « Réglages WebRX » : réseaux Wi-Fi connus et adresse du serveur */
/* ------------------------------------------------------------------ */
static int s_page;                         /* OTA_PAGE_* : page servie sur « / » */
#define SCAN_MAX 16
static char s_scan[SCAN_MAX][33];          /* réseaux vus au démarrage du point d'accès */
static int s_nscan;

void ota_set_page(int page) { s_page = page; }

static const char WRX_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WebRX</title><style>"
    "body{font:16px -apple-system,system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:24px;max-width:480px}"
    "h1{font-size:22px;margin:0 0 4px}p{color:#9aa}h2{font-size:15px;color:#9aa;margin:20px 0 4px;font-weight:600}"
    "input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;margin:4px 0;background:#1d2026;"
    "color:#eee;border:1px solid #3a3d44;border-radius:8px}"
    "button{font-size:17px;padding:12px 20px;border:0;border-radius:10px;background:#3b82f6;color:#fff;width:100%;margin-top:20px}"
    "button:disabled{background:#334}#m{margin-top:12px;min-height:1.4em}.ok{color:#4ade80}.ko{color:#f87171}"
    "#l{float:right;background:none;border:1px solid #445;color:#9aa;width:auto;padding:4px 10px;font-size:13px;margin:0}"
    "a{color:#9aa}"
    "</style></head><body><button id=l type=button onclick=sw()></button><h1 id=t></h1><p id=h></p><form id=f onsubmit='return go()'>";
static const char WRX_TAIL[] =
    "<button id=b></button></form><div id=m></div><p><a id=o href=/update.html></a></p><script>"
    "var T={fr:{t:'WebRX · réglages',h:'Réseaux Wi-Fi que la montre peut utiliser (dans l’ordre de préférence) "
    "et adresse de ton serveur OpenWebRX, par exemple <b>https://sdr.exemple.fr/</b>.',"
    "n:'Wi-Fi ',s:'Nom du réseau',p:'Mot de passe',k:'•••• enregistré (vide = inchangé)',"
    "u:'Serveur OpenWebRX',b:'Enregistrer',w:'Enregistrement…',ok:'Enregistré : la montre redémarre sur WebRX.',"
    "ko:'Échec : ',lost:'Connexion perdue.',url:'l’adresse doit commencer par http:// ou https://',"
    "net:'indiquer au moins un réseau',size:'champ trop long',nvs:'écriture impossible',o:'Mise à jour du firmware →'},"
    "en:{t:'WebRX · settings',h:'Wi-Fi networks the watch may use (in order of preference) "
    "and the address of your OpenWebRX server, for example <b>https://sdr.example.org/</b>.',"
    "n:'Wi-Fi ',s:'Network name',p:'Password',k:'•••• saved (empty = unchanged)',"
    "u:'OpenWebRX server',b:'Save',w:'Saving…',ok:'Saved: the watch restarts into WebRX.',"
    "ko:'Failed: ',lost:'Connection lost.',url:'the address must start with http:// or https://',"
    "net:'enter at least one network',size:'field too long',nvs:'could not save',o:'Firmware update →'}};"
    "var L=(navigator.language||'en').slice(0,2)=='fr'?'fr':'en';"
    "function $(i){return document.getElementById(i);}"
    "function tr(k){return T[L][k]||k;}"
    "function draw(){document.documentElement.lang=L;['t','b','u','o'].forEach(function(k){$(k).textContent=tr(k);});"
    "$('h').innerHTML=tr('h');$('l').textContent=L=='fr'?'English':'Français';"
    "for(var i=0;i<3;i++){$('n'+i).textContent=tr('n')+(i+1);var s=$('s'+i),p=$('p'+i);s.placeholder=tr('s');"
    "p.placeholder=p.dataset.k?tr('k'):tr('p');}}"
    "function sw(){L=L=='fr'?'en':'fr';draw();}"
    "function go(){var f=$('f'),m=$('m'),b=$('b'),q=[];"
    "for(var i=0;i<f.elements.length;i++){var e=f.elements[i];if(e.name)q.push(e.name+'='+encodeURIComponent(e.value));}"
    "b.disabled=true;m.className='';m.textContent=tr('w');"
    "var x=new XMLHttpRequest();x.open('POST','/webrx');"
    "x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
    "x.onload=function(){if(x.status==200){m.className='ok';m.textContent=tr('ok');}"
    "else{m.className='ko';m.textContent=tr('ko')+tr(x.responseText);b.disabled=false;}};"
    "x.onerror=function(){m.className='ko';m.textContent=tr('lost');b.disabled=false;};"
    "x.send(q.join('&'));return false;}draw();</script></body></html>";

/* copie échappée pour un attribut HTML entre apostrophes */
static size_t esc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        const char *r = NULL;
        switch (*s) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '\'': r = "&#39;"; break;
        case '"': r = "&quot;"; break;
        }
        size_t l = r ? strlen(r) : 1;
        if (n + l + 1 > cap) break;
        if (r) memcpy(o + n, r, l); else o[n] = *s;
        n += l;
    }
    o[n] = 0;
    return n;
}

static esp_err_t webrx_get(httpd_req_t *r)
{
    s_last_activity_us = esp_timer_get_time();
    static wrx_cfg_t c;
    wrx_cfg_load(&c);
    const size_t cap = sizeof(WRX_HEAD) + sizeof(WRX_TAIL) + 4096;
    char *pg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pg) pg = malloc(cap);
    if (!pg) return httpd_resp_send_500(r);
    size_t n = strlcpy(pg, WRX_HEAD, cap);
    char e[3 * sizeof c.url];
    for (int i = 0; i < WRX_NETS; i++) {
        esc(e, sizeof e, c.ssid[i]);
        n += snprintf(pg + n, cap - n,
                      "<h2 id=n%d></h2><input id=s%d name=s%d list=ss maxlength=32 autocapitalize=off "
                      "autocomplete=off value='%s'><input id=p%d name=p%d type=password maxlength=64%s>",
                      i, i, i, e, i, i, c.pass[i][0] ? " data-k=1" : "");
    }
    esc(e, sizeof e, c.url);
    n += snprintf(pg + n, cap - n,
                  "<h2 id=u></h2><input name=u type=url maxlength=190 autocapitalize=off "
                  "placeholder='https://' value='%s'><datalist id=ss>", e);
    for (int i = 0; i < s_nscan && n < cap - 200; i++) {
        esc(e, sizeof e, s_scan[i]);
        n += snprintf(pg + n, cap - n, "<option value='%s'>", e);
    }
    n += snprintf(pg + n, cap - n, "</datalist>%s", WRX_TAIL);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Connection", "close");
    esp_err_t ret = httpd_resp_send(r, pg, n < cap ? n : cap - 1);
    free(pg);
    return ret;
}

static esp_err_t root_get(httpd_req_t *r)
{
    return s_page == OTA_PAGE_WEBRX ? webrx_get(r) : page_get(r);
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* valeur décodée du champ `key` d'un corps x-www-form-urlencoded :
 * 1 trouvée, 0 absente (out vide), -1 trop longue */
static int form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        p += kl + 1;
        size_t n = 0;
        while (*p && *p != '&') {
            char c = *p++;
            if (c == '+') c = ' ';
            else if (c == '%' && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) { c = (char)(hexv(p[0]) * 16 + hexv(p[1])); p += 2; }
            if (n + 1 >= cap) { out[0] = 0; return -1; }
            out[n++] = c;
        }
        out[n] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}

static esp_err_t webrx_bad(httpd_req_t *r, const char *code)
{
    httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, code);
}

static esp_err_t webrx_post(httpd_req_t *r)
{
    s_last_activity_us = esp_timer_get_time();
    #define BODY_MAX 1536
    static char *body;                     /* en PSRAM, gardé pour la session */
    if (!body) body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return webrx_bad(r, "nvs");
    if (r->content_len <= 0 || r->content_len >= BODY_MAX) return webrx_bad(r, "size");
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0) return webrx_bad(r, "net");
        got += n;
    }
    body[got] = 0;
    static wrx_cfg_t old, c;
    wrx_cfg_load(&old);
    memset(&c, 0, sizeof c);
    bool ok = true, any = false;
    for (int i = 0; i < WRX_NETS; i++) {
        char k[4] = { 's', (char)('0' + i), 0 };
        if (form_get(body, k, c.ssid[i], sizeof c.ssid[i]) < 0) ok = false;
        k[0] = 'p';
        if (form_get(body, k, c.pass[i], sizeof c.pass[i]) < 0) ok = false;
        /* mot de passe laissé vide pour un réseau déjà enregistré : on le garde */
        if (!c.pass[i][0] && c.ssid[i][0]) {
            for (int j = 0; j < WRX_NETS; j++)
                if (!strcmp(old.ssid[j], c.ssid[i])) { strlcpy(c.pass[i], old.pass[j], sizeof c.pass[i]); break; }
        }
        if (!c.ssid[i][0]) c.pass[i][0] = 0;
        any |= c.ssid[i][0] != 0;
    }
    if (form_get(body, "u", c.url, sizeof c.url) < 0) ok = false;
    if (!ok) return webrx_bad(r, "size");
    /* espaces autour de l'adresse (copier-coller) */
    char *u = c.url;
    while (*u == ' ') u++;
    memmove(c.url, u, strlen(u) + 1);
    for (size_t l = strlen(c.url); l && c.url[l - 1] == ' '; l--) c.url[l - 1] = 0;
    if (strncmp(c.url, "http://", 7) && strncmp(c.url, "https://", 8)) return webrx_bad(r, "url");
    if (!any) return webrx_bad(r, "net");
    /* réseaux tassés vers le haut (ordre de préférence conservé) */
    for (int i = 0, j = 0; i < WRX_NETS; i++) {
        if (!c.ssid[i][0]) continue;
        if (i != j) {
            memcpy(c.ssid[j], c.ssid[i], sizeof c.ssid[j]);
            memcpy(c.pass[j], c.pass[i], sizeof c.pass[j]);
            c.ssid[i][0] = c.pass[i][0] = 0;
        }
        j++;
    }
    if (wrx_cfg_save(&c) != ESP_OK) return webrx_bad(r, "nvs");
    ESP_LOGI(TAG, "réglages WebRX enregistrés");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Connection", "close");
    httpd_resp_sendstr(r, "ok");
    post_ev(OTA_EV_SAVED);
    return ESP_OK;
}

/* réseaux visibles, proposés dans la page (point d'accès + station) */
static void scan_networks(void)
{
    s_nscan = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = 20;
    wifi_ap_record_t *rec = heap_caps_calloc(n, sizeof *rec, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rec) { esp_wifi_clear_ap_list(); return; }
    if (esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        for (int i = 0; i < n && s_nscan < SCAN_MAX; i++) {
            const char *ss = (const char *)rec[i].ssid;
            if (!ss[0]) continue;
            bool dup = false;
            for (int j = 0; j < s_nscan; j++) dup |= !strcmp(s_scan[j], ss);
            if (!dup) strlcpy(s_scan[s_nscan++], ss, sizeof s_scan[0]);
        }
    }
    free(rec);
}

/* toute autre adresse renvoie la page (pratique si l'iPhone teste le réseau) */

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
    if (s_page == OTA_PAGE_UPDATE && !power_vbus() && power_percent() < MIN_BATT_PCT) return N_("Batterie trop faible (30 % mini) : brancher l'USB");
    if (!esp_ota_get_next_update_partition(NULL)) return N_("Table de partitions sans OTA : un flash USB est nécessaire");

    ble_stop();                                  /* libère la radio et la RAM du Bluetooth */
    s_ble_stopped = true;
    /* CPU (et donc bus APB) à pleine vitesse pendant toute la session : sinon
     * l'économiseur redescend à 10 MHz 1,5 s après le dernier toucher et le
     * Wi-Fi rame. Libéré par le redémarrage qui termine toujours la session. */
    power_hold_active(true);

    static bool net_inited;
    if (!net_inited) {
        if (net_base_init() != ESP_OK) return N_("Erreur réseau (boucle d'événements)");
        esp_netif_t *apif = net_ap_netif();
        if (!apif) return N_("Erreur réseau (boucle d'événements)");
        /* le DHCP annonce la montre comme serveur DNS (voir dns_task) */
        esp_netif_dhcps_stop(apif);
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        esp_netif_set_dns_info(apif, ESP_NETIF_DNS_MAIN, &dns);
        uint8_t offer_dns = 1;
        esp_netif_dhcps_option(apif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
        esp_netif_dhcps_start(apif);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event, NULL);
        net_inited = true;
    }
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wc) != ESP_OK) return N_("Erreur Wi-Fi (mémoire ?)");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    load_or_create_password();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, OTA_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(OTA_SSID);
    strlcpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    /* réglages WebRX : station en plus, pour proposer les réseaux visibles */
    esp_wifi_set_mode(s_page == OTA_PAGE_WEBRX ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) return N_("Le point d'accès n'a pas démarré");
    if (s_page == OTA_PAGE_WEBRX) scan_networks();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.max_uri_handlers = 12;
    hc.recv_wait_timeout = 10;
    hc.lru_purge_enable = true;        /* Safari ouvre des connexions en avance : on recycle */
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_open_sockets = 4;           /* chaque socket coûte de la RAM interne */
    esp_err_t he = httpd_start(&s_httpd, &hc);
    if (he != ESP_OK) {
        /* note la cause et la mémoire dans le journal (À propos), puis un
         * second essai plus sobre */
        ble_diag("OTA serveur web : %s, libre %u Ko, bloc %u Ko", esp_err_to_name(he),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        hc.stack_size = 4608;
        hc.max_open_sockets = 2;
        he = httpd_start(&s_httpd, &hc);
        if (he != ESP_OK) return N_("Le serveur web n'a pas démarré");
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/update", .method = HTTP_POST, .handler = update_post },
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/index.html", .method = HTTP_GET, .handler = root_get },
        { .uri = "/update.html", .method = HTTP_GET, .handler = page_get },
        { .uri = "/webrx", .method = HTTP_GET, .handler = webrx_get },
        { .uri = "/webrx", .method = HTTP_POST, .handler = webrx_post },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = probe_apple },
        { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = probe_apple },
        { .uri = "/generate_204", .method = HTTP_GET, .handler = probe_204 },
        { .uri = "/*", .method = HTTP_GET, .handler = not_found },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    s_active = true;                   /* avant la tâche DNS, qui boucle tant que actif */
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);

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
    if (s_finishing) return;               /* image en cours d'activation : on laisse finir */
    ESP_LOGI(TAG, "mise à jour abandonnée");
    if (s_receiving) {
        /* La tâche httpd est dans update_post, le client envoie encore :
         * httpd_stop() attendrait la fin du transfert, à l'infini (montre
         * figée). Redémarrage direct : l'image partielle est dans
         * l'emplacement inactif, jamais activé, rien à défaire. */
        esp_restart();
    }
    ota_reboot_now();
}

bool ota_active(void) { return s_active; }
const char *ota_ssid(void) { return OTA_SSID; }
const char *ota_password(void) { return s_pass; }
const char *ota_new_version(void) { return s_new_ver; }
