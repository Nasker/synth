#include "web.h"
#include "synth.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define AP_SSID "esp32-synth"
#define NUM_PRESETS  8
#define PRESET_MAGIC 0x53595250   // 'SYRP'

typedef struct {
    uint32_t      magic;
    char          name[16];
    synth_patch_t patch[SYNTH_NUM_PARTS];
    synth_fx_t    fx;
} preset_blob_t;

static const char *TAG = "web";
static volatile bool s_user_played;
static nvs_handle_t s_nvs;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

bool web_user_played(void)
{
    return s_user_played;
}

static void apply_param(const char *name, double value, int part)
{
    // patch params route to the addressed part; fx params are global
    synth_patch_t *p = synth_patch(part);
    synth_fx_t    *f = synth_fx_params();
    if      (!strcmp(name, "wave1"))     p->wave1      = (int)value;
    else if (!strcmp(name, "wave2"))     p->wave2      = (int)value;
    else if (!strcmp(name, "osc_mix"))   p->osc_mix    = value;
    else if (!strcmp(name, "pwm"))       p->pwm        = value;
    else if (!strcmp(name, "detune"))    p->detune     = value;
    else if (!strcmp(name, "spread"))    p->spread     = value;
    else if (!strcmp(name, "cutoff"))    p->cutoff     = value;
    else if (!strcmp(name, "reso"))      p->reso       = value;
    else if (!strcmp(name, "lfo_rate"))  p->lfo_rate   = value;
    else if (!strcmp(name, "lfo_depth")) p->lfo_depth  = value;
    else if (!strcmp(name, "attack"))    p->attack_s   = value;
    else if (!strcmp(name, "decay"))     p->decay_s    = value;
    else if (!strcmp(name, "sustain"))   p->sustain    = value;
    else if (!strcmp(name, "release"))   p->release_s  = value;
    else if (!strcmp(name, "glide"))     p->glide_s    = value;
    else if (!strcmp(name, "chorus_rate")) f->chorus_rate  = value;
    else if (!strcmp(name, "chorus_depth"))f->chorus_depth = value;
    else if (!strcmp(name, "chorus_mix"))  f->chorus_mix   = value;
    else if (!strcmp(name, "rev_room"))    f->rev_room     = value;
    else if (!strcmp(name, "rev_damp"))    f->rev_damp     = value;
    else if (!strcmp(name, "rev_wet"))     f->rev_wet      = value;
    else if (!strcmp(name, "delay_ms"))  f->delay_ms   = value;
    else if (!strcmp(name, "delay_fb"))  f->delay_fb   = value;
    else if (!strcmp(name, "delay_mix")) f->delay_mix  = value;
    else if (!strcmp(name, "delay_damp"))f->delay_damp = value;
    else if (!strcmp(name, "comp_thresh"))f->comp_thresh = value;
    else if (!strcmp(name, "comp_ratio")) f->comp_ratio  = value;
    else if (!strcmp(name, "master"))    f->master     = value;
}

static void preset_save(int slot, const char *name)
{
    if (slot < 0 || slot >= NUM_PRESETS) return;
    preset_blob_t b = {0};
    b.magic = PRESET_MAGIC;
    strlcpy(b.name, name && *name ? name : "preset", sizeof(b.name));
    memcpy(b.patch, synth_patch(0), sizeof(b.patch));
    memcpy(&b.fx, synth_fx_params(), sizeof(b.fx));
    char key[8];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_set_blob(s_nvs, key, &b, sizeof(b));
    nvs_set_u8(s_nvs, "last", (uint8_t)slot);
    nvs_commit(s_nvs);
}

static bool preset_load(int slot)
{
    if (slot < 0 || slot >= NUM_PRESETS) return false;
    preset_blob_t b;
    char key[8];
    snprintf(key, sizeof(key), "p%d", slot);
    size_t len = sizeof(b);
    if (nvs_get_blob(s_nvs, key, &b, &len) != ESP_OK || b.magic != PRESET_MAGIC) {
        return false;
    }
    memcpy(synth_patch(0), b.patch, sizeof(b.patch));
    memcpy(synth_fx_params(), &b.fx, sizeof(b.fx));
    nvs_set_u8(s_nvs, "last", (uint8_t)slot);
    nvs_commit(s_nvs);
    return true;
}

// full state push: parts + fx, so the UI can populate both part tabs
static esp_err_t send_full_state(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON *parts = cJSON_AddArrayToObject(root, "p");
    for (int i = 0; i < SYNTH_NUM_PARTS; i++) {
        const synth_patch_t *pp = synth_patch(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "wave1",     pp->wave1);
        cJSON_AddNumberToObject(o, "wave2",     pp->wave2);
        cJSON_AddNumberToObject(o, "osc_mix",   pp->osc_mix);
        cJSON_AddNumberToObject(o, "pwm",       pp->pwm);
        cJSON_AddNumberToObject(o, "detune",    pp->detune);
        cJSON_AddNumberToObject(o, "spread",    pp->spread);
        cJSON_AddNumberToObject(o, "cutoff",    pp->cutoff);
        cJSON_AddNumberToObject(o, "reso",      pp->reso);
        cJSON_AddNumberToObject(o, "lfo_rate",  pp->lfo_rate);
        cJSON_AddNumberToObject(o, "lfo_depth", pp->lfo_depth);
        cJSON_AddNumberToObject(o, "attack",    pp->attack_s);
        cJSON_AddNumberToObject(o, "decay",     pp->decay_s);
        cJSON_AddNumberToObject(o, "sustain",   pp->sustain);
        cJSON_AddNumberToObject(o, "release",   pp->release_s);
        cJSON_AddNumberToObject(o, "glide",     pp->glide_s);
        cJSON_AddItemToArray(parts, o);
    }
    const synth_fx_t *fx = synth_fx_params();
    cJSON *f = cJSON_AddObjectToObject(root, "fx");
    cJSON_AddNumberToObject(f, "chorus_rate",  fx->chorus_rate);
    cJSON_AddNumberToObject(f, "chorus_depth", fx->chorus_depth);
    cJSON_AddNumberToObject(f, "chorus_mix",   fx->chorus_mix);
    cJSON_AddNumberToObject(f, "rev_room",     fx->rev_room);
    cJSON_AddNumberToObject(f, "rev_damp",     fx->rev_damp);
    cJSON_AddNumberToObject(f, "rev_wet",      fx->rev_wet);
    cJSON_AddNumberToObject(f, "delay_ms",    fx->delay_ms);
    cJSON_AddNumberToObject(f, "delay_fb",    fx->delay_fb);
    cJSON_AddNumberToObject(f, "delay_mix",   fx->delay_mix);
    cJSON_AddNumberToObject(f, "delay_damp",  fx->delay_damp);
    cJSON_AddNumberToObject(f, "comp_thresh", fx->comp_thresh);
    cJSON_AddNumberToObject(f, "comp_ratio",  fx->comp_ratio);
    cJSON_AddNumberToObject(f, "master",      fx->master);

    cJSON *pl = cJSON_AddArrayToObject(root, "presets");
    for (int i = 0; i < NUM_PRESETS; i++) {
        preset_blob_t b;
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        size_t len = sizeof(b);
        bool used = nvs_get_blob(s_nvs, key, &b, &len) == ESP_OK &&
                    b.magic == PRESET_MAGIC;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "used", used);
        cJSON_AddStringToObject(o, "name", used ? b.name : "");
        cJSON_AddItemToArray(pl, o);
    }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;
    httpd_ws_frame_t out = {
        .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)txt, .len = strlen(txt),
    };
    esp_err_t ret = httpd_ws_send_frame_async(req->handle, httpd_req_to_sockfd(req), &out);
    free(txt);
    return ret;
}

static void handle_ws_msg(httpd_req_t *req, const char *msg)
{
    cJSON *j = cJSON_Parse(msg);
    if (!j) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(j); return; }

    const cJSON *pj = cJSON_GetObjectItem(j, "part");
    int part = cJSON_IsNumber(pj) ? pj->valueint : 0;

    if (!strcmp(type->valuestring, "note_on")) {
        const cJSON *note = cJSON_GetObjectItem(j, "note");
        const cJSON *vel  = cJSON_GetObjectItem(j, "vel");
        if (cJSON_IsNumber(note)) {
            s_user_played = true;
            synth_note_on(note->valueint, cJSON_IsNumber(vel) ? vel->valuedouble : 0.8, part);
        }
    } else if (!strcmp(type->valuestring, "note_off")) {
        const cJSON *note = cJSON_GetObjectItem(j, "note");
        if (cJSON_IsNumber(note)) synth_note_off(note->valueint, part);
    } else if (!strcmp(type->valuestring, "param")) {
        const cJSON *name = cJSON_GetObjectItem(j, "name");
        const cJSON *val  = cJSON_GetObjectItem(j, "value");
        if (cJSON_IsString(name) && cJSON_IsNumber(val)) {
            apply_param(name->valuestring, val->valuedouble, part);
        }
    } else if (!strcmp(type->valuestring, "get_state")) {
        send_full_state(req);
    } else if (!strcmp(type->valuestring, "preset")) {
        const cJSON *op   = cJSON_GetObjectItem(j, "op");
        const cJSON *slot = cJSON_GetObjectItem(j, "slot");
        if (cJSON_IsString(op) && cJSON_IsNumber(slot)) {
            if (!strcmp(op->valuestring, "save")) {
                const cJSON *nm = cJSON_GetObjectItem(j, "name");
                preset_save(slot->valueint,
                            cJSON_IsString(nm) ? nm->valuestring : NULL);
                send_full_state(req);   // refresh slot names
            } else if (!strcmp(op->valuestring, "load")) {
                if (preset_load(slot->valueint)) send_full_state(req);
            }
        }
    }
    cJSON_Delete(j);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t ws_get(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;  // completes the handshake
    }
    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);  // fetch length first
    if (ret != ESP_OK) return ret;
    if (pkt.len == 0 || pkt.len > 512) return ESP_OK;

    uint8_t *buf = calloc(1, pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK) handle_ws_msg(req, (char *)buf);
    free(buf);
    return ret;
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    cfg.core_id = 0;   // keep HTTP/WS off the audio core
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));

    const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_get,
    };
    const httpd_uri_t uri_ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_get,
        .is_websocket = true,
    };
    httpd_register_uri_handler(srv, &uri_index);
    httpd_register_uri_handler(srv, &uri_ws);
}

static void start_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len        = strlen(AP_SSID);
    ap.ap.channel         = 1;
    ap.ap.authmode        = WIFI_AUTH_OPEN;
    ap.ap.max_connection  = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void web_start(void)
{
    start_ap();
    nvs_open("synth", NVS_READWRITE, &s_nvs);
    uint8_t last;
    if (nvs_get_u8(s_nvs, "last", &last) == ESP_OK && preset_load(last)) {
        ESP_LOGI(TAG, "restored preset slot %d", last);
    }
    start_httpd();
    ESP_LOGI(TAG, "AP '%s' up — connect and browse to http://192.168.4.1", AP_SSID);
}
