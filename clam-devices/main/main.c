/*
 * DEEPLOVE ROBOT - Firmware provisioning + claim theo BACKEND (production).
 * -------------------------------------------------------------------------
 * Luồng (khớp app DeepLove + backend deeplove-agent-api):
 *   1) Lần đầu bật: sinh danh tính lưu NVS:
 *        - device_id   : "ESP32C3_<MAC>" (public, = hardware_id trên backend)
 *        - claim_code  : 6 ký tự cho chủ máy
 *        - cặp khóa Ed25519 (pub 32B / priv 64B) để KÝ khi hoàn tất claim
 *        - setup_ap_ssid: "Plant-<4 hex cuối MAC>" (khớp default backend)
 *   2) Phát AP <setup_ap_ssid> + web server:
 *        - GET  /          : trang claim + QR {"id","code"}
 *        - GET  /identity  : JSON danh tính (cho công cụ factory đăng ký backend)
 *        - GET  /scan      : danh sách WiFi robot thấy (app cho user chọn)
 *        - POST /provision : app gửi {claim_session_id, home_wifi_ssid,
 *                            home_wifi_password, backend_url} -> lưu NVS + reboot
 *   3) Có WiFi -> nối mạng, đồng bộ giờ (SNTP), rồi:
 *        - Nếu đã có MQTT creds (đã claim trước) -> nối EMQX luôn.
 *        - Nếu có claim_session_id -> gọi HTTPS
 *          POST {backend_url}/api/v1/device/claim-sessions/{id}/complete
 *          (ký Ed25519 "device_id|session_id|nonce|timestamp") -> nhận
 *          device_token + thông tin MQTT -> lưu NVS -> nối EMQX.
 *   Giữ nút BOOT 3s => factory reset (xóa danh tính -> sinh mới).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "qrcode.h"
#include "mqtt_client.h"
#include "mbedtls/base64.h"
#include "sodium.h"

static const char *TAG = "deeplove";

#define FIRMWARE_VERSION "1.0.0"

#define NVS_NAMESPACE   "device"

// Nut BOOT (GPIO9) - giu ~3s luc chay => factory reset.
#define BOOT_BUTTON_GPIO   9
#define RESET_HOLD_SECONDS 3

// Mat khau AP setup (SSID theo MAC). AP chi dung luc them thiet bi.
#define AP_PASS     "12345678"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

// Bang ky tu claim_code (bo O/0, I/1 de do nham).
static const char CLAIM_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define CLAIM_CODE_LEN   6

// ----- Danh tinh (RAM) -----
static char    device_id[32];                   // ESP32C3_<MAC>
static char    claim_code[CLAIM_CODE_LEN + 1];  // 6 ky tu
static char    setup_ap_ssid[24];               // Plant-XXXX
static uint8_t ed_pub[crypto_sign_PUBLICKEYBYTES];   // 32
static uint8_t ed_priv[crypto_sign_SECRETKEYBYTES];  // 64

// ----- Cau hinh mang (NVS) -----
static char g_wifi_ssid[33];
static char g_wifi_pass[65];
static char g_backend_url[128];        // vd https://services.runagent.io/deeplove-agent-api
static char g_claim_session_id[64];    // phien claim dang cho (rong = khong co)

// ----- MQTT creds (NVS, co sau khi complete) -----
static char g_mqtt_host[96];
static char g_mqtt_port[8];
static char g_mqtt_user[48];
static char g_mqtt_pass[96];           // device_token
static char g_mqtt_client[64];
static char g_mqtt_topic[96];          // topic_signal

static esp_ip4_addr_t s_sta_ip = {0};
static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_time_synced = false;

static void mqtt_app_start(void);

// ============================================================
//  BASE64 (chuan, khop base64.StdEncoding cua backend Go)
// ============================================================
static bool b64_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap)
{
    size_t olen = 0;
    int r = mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, inlen);
    if (r != 0) {
        return false;
    }
    out[olen] = '\0';
    return true;
}

// Trich gia tri chuoi cua "key":"value" trong JSON phang (input do backend/app
// tao, shape co dinh). Chi lay gia tri kieu chuoi. Tra ve true neu tim thay.
static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && p[1]) p++;   // bo qua escape don gian
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// ============================================================
//  NVS: danh tinh + cau hinh
// ============================================================
static void generate_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id),
             "ESP32C3_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // setup_ap_ssid = Plant-<2 byte cuoi MAC> (khop default backend: 4 hex cuoi).
    snprintf(setup_ap_ssid, sizeof(setup_ap_ssid), "Plant-%02X%02X", mac[4], mac[5]);
}

static void generate_claim_code(void)
{
    int n = sizeof(CLAIM_ALPHABET) - 1;
    for (int i = 0; i < CLAIM_CODE_LEN; i++) {
        claim_code[i] = CLAIM_ALPHABET[esp_random() % n];
    }
    claim_code[CLAIM_CODE_LEN] = '\0';
}

// Sinh moi danh tinh + cap khoa Ed25519, luu NVS.
static void create_and_save_identity(nvs_handle_t nvs)
{
    ESP_LOGW(TAG, "Lan dau khoi dong -> sinh danh tinh + khoa Ed25519 moi...");
    generate_device_id();
    generate_claim_code();
    crypto_sign_keypair(ed_pub, ed_priv);   // libsodium

    ESP_ERROR_CHECK(nvs_set_str(nvs, "device_id", device_id));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "claim_code", claim_code));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ap_ssid", setup_ap_ssid));
    ESP_ERROR_CHECK(nvs_set_blob(nvs, "ed_pub", ed_pub, sizeof(ed_pub)));
    ESP_ERROR_CHECK(nvs_set_blob(nvs, "ed_priv", ed_priv, sizeof(ed_priv)));
    ESP_ERROR_CHECK(nvs_commit(nvs));
}

static void load_identity(nvs_handle_t nvs)
{
    size_t len;
    // device_id: tinh lai tu MAC de chac chan (dong thoi set setup_ap_ssid).
    generate_device_id();
    len = sizeof(device_id);
    nvs_get_str(nvs, "device_id", device_id, &len);   // uu tien ban da luu neu co

    len = sizeof(claim_code);
    if (nvs_get_str(nvs, "claim_code", claim_code, &len) != ESP_OK) {
        generate_claim_code();
        nvs_set_str(nvs, "claim_code", claim_code);
    }

    len = sizeof(setup_ap_ssid);
    if (nvs_get_str(nvs, "ap_ssid", setup_ap_ssid, &len) != ESP_OK) {
        nvs_set_str(nvs, "ap_ssid", setup_ap_ssid);   // luu ban vua tinh tu MAC
    }

    // Khoa Ed25519: neu thieu (migrate tu firmware cu) -> sinh moi + luu, KHONG abort.
    len = sizeof(ed_pub);
    esp_err_t e1 = nvs_get_blob(nvs, "ed_pub", ed_pub, &len);
    len = sizeof(ed_priv);
    esp_err_t e2 = nvs_get_blob(nvs, "ed_priv", ed_priv, &len);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "Chua co khoa Ed25519 trong NVS -> sinh moi (migrate).");
        crypto_sign_keypair(ed_pub, ed_priv);
        nvs_set_blob(nvs, "ed_pub", ed_pub, sizeof(ed_pub));
        nvs_set_blob(nvs, "ed_priv", ed_priv, sizeof(ed_priv));
    }
    nvs_commit(nvs);
}

static void device_identity_init(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));

    size_t len = sizeof(device_id);
    esp_err_t err = nvs_get_str(nvs, "device_id", device_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        create_and_save_identity(nvs);
    } else {
        ESP_ERROR_CHECK(err);
        load_identity(nvs);
        ESP_LOGI(TAG, "Da co danh tinh trong flash -> doc lai.");
    }
    nvs_close(nvs);
}

// Doc cau hinh mang + MQTT creds tu NVS (rong neu chua co).
static void config_load(void)
{
    g_wifi_ssid[0] = g_wifi_pass[0] = g_backend_url[0] = g_claim_session_id[0] = '\0';
    g_mqtt_host[0] = g_mqtt_port[0] = g_mqtt_user[0] = '\0';
    g_mqtt_pass[0] = g_mqtt_client[0] = g_mqtt_topic[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(g_wifi_ssid);        nvs_get_str(nvs, "wifi_ssid", g_wifi_ssid, &len);
        len = sizeof(g_wifi_pass);        nvs_get_str(nvs, "wifi_pass", g_wifi_pass, &len);
        len = sizeof(g_backend_url);      nvs_get_str(nvs, "backend", g_backend_url, &len);
        len = sizeof(g_claim_session_id); nvs_get_str(nvs, "claim_sid", g_claim_session_id, &len);
        len = sizeof(g_mqtt_host);        nvs_get_str(nvs, "mq_host", g_mqtt_host, &len);
        len = sizeof(g_mqtt_port);        nvs_get_str(nvs, "mq_port", g_mqtt_port, &len);
        len = sizeof(g_mqtt_user);        nvs_get_str(nvs, "mq_user", g_mqtt_user, &len);
        len = sizeof(g_mqtt_pass);        nvs_get_str(nvs, "mq_pass", g_mqtt_pass, &len);
        len = sizeof(g_mqtt_client);      nvs_get_str(nvs, "mq_client", g_mqtt_client, &len);
        len = sizeof(g_mqtt_topic);       nvs_get_str(nvs, "mq_topic", g_mqtt_topic, &len);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Config: WiFi='%s' backend='%s' claim_sid='%s' mqtt=%s",
             g_wifi_ssid, g_backend_url, g_claim_session_id,
             g_mqtt_pass[0] ? "co" : "chua");
}

// Luu WiFi + backend + claim_session_id (khi app POST /provision).
static void config_save_provision(const char *ssid, const char *pass,
                                  const char *backend, const char *sid)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (ssid)    nvs_set_str(nvs, "wifi_ssid", ssid);
        if (pass)    nvs_set_str(nvs, "wifi_pass", pass);
        if (backend) nvs_set_str(nvs, "backend", backend);
        if (sid)     nvs_set_str(nvs, "claim_sid", sid);
        // Provision moi -> bo MQTT creds cu (se claim lai).
        nvs_erase_key(nvs, "mq_pass");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Luu MQTT creds sau khi complete claim thanh cong. Xoa claim_sid (dung 1 lan).
static void config_save_mqtt(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "mq_host", g_mqtt_host);
        nvs_set_str(nvs, "mq_port", g_mqtt_port);
        nvs_set_str(nvs, "mq_user", g_mqtt_user);
        nvs_set_str(nvs, "mq_pass", g_mqtt_pass);
        nvs_set_str(nvs, "mq_client", g_mqtt_client);
        nvs_set_str(nvs, "mq_topic", g_mqtt_topic);
        nvs_erase_key(nvs, "claim_sid");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    g_claim_session_id[0] = '\0';
}

static void device_factory_reset(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "*** FACTORY RESET *** Da xoa danh tinh -> sinh moi luc boot.");
}

// ============================================================
//  Nut BOOT
// ============================================================
static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}
static bool button_pressed(void) { return gpio_get_level(BOOT_BUTTON_GPIO) == 0; }

static void print_identity(void)
{
    char pub_b64[80];
    b64_encode(ed_pub, sizeof(ed_pub), pub_b64, sizeof(pub_b64));
    ESP_LOGI(TAG, "=========== DEVICE IDENTITY ===========");
    ESP_LOGI(TAG, " device_id  : %s", device_id);
    ESP_LOGI(TAG, " claim_code : %s", claim_code);
    ESP_LOGI(TAG, " ap_ssid    : %s", setup_ap_ssid);
    ESP_LOGI(TAG, " public_key : %s", pub_b64);
    if (g_mqtt_pass[0]) {
        ESP_LOGI(TAG, " claim      : DA claim (co MQTT creds)");
    } else if (g_claim_session_id[0]) {
        ESP_LOGI(TAG, " claim      : dang cho hoan tat (session=%s)", g_claim_session_id);
    } else {
        ESP_LOGI(TAG, " claim      : CHUA (cho app provision)");
    }
    if (s_sta_ip.addr != 0) {
        ESP_LOGI(TAG, " WiFi nha   : %s  IP " IPSTR, g_wifi_ssid, IP2STR(&s_sta_ip));
    } else if (g_wifi_ssid[0]) {
        ESP_LOGI(TAG, " WiFi nha   : %s  (dang noi...)", g_wifi_ssid);
    } else {
        ESP_LOGI(TAG, " WiFi nha   : (chua cau hinh)");
    }
    ESP_LOGI(TAG, "=======================================");
}

// ============================================================
//  WEB + QR
// ============================================================
static char s_svg[32768];
static size_t s_svg_len;

static void svg_append(const char *s)
{
    size_t n = strlen(s);
    if (s_svg_len + n < sizeof(s_svg) - 1) {
        memcpy(s_svg + s_svg_len, s, n);
        s_svg_len += n;
        s_svg[s_svg_len] = '\0';
    }
}

static void qr_to_svg(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    const int scale = 8, quiet = 4;
    int dim = (size + quiet * 2) * scale;
    char tmp[160];
    snprintf(tmp, sizeof(tmp),
             "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' shape-rendering='crispEdges'>",
             dim, dim);
    svg_append(tmp);
    svg_append("<rect width='100%' height='100%' fill='white'/>");
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                snprintf(tmp, sizeof(tmp),
                         "<rect x='%d' y='%d' width='%d' height='%d' fill='black'/>",
                         (x + quiet) * scale, (y + quiet) * scale, scale, scale);
                svg_append(tmp);
            }
        }
    }
    svg_append("</svg>");
}

static void build_qr_svg(const char *text)
{
    s_svg_len = 0; s_svg[0] = '\0';
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_to_svg;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, text);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"code\":\"%s\"}", device_id, claim_code);
    build_qr_svg(payload);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Claim Device</title><style>"
        "body{font-family:sans-serif;text-align:center;background:#f0f2f5;margin:0;padding:24px}"
        ".card{background:#fff;max-width:360px;margin:auto;padding:24px;border-radius:16px;box-shadow:0 4px 12px #0002}"
        ".code{font-size:32px;font-weight:bold;letter-spacing:4px;color:#1565c0;margin:8px 0}"
        ".id{font-size:13px;color:#666;word-break:break-all}"
        "</style></head><body><div class='card'>"
        "<h2>DeepLove Robot</h2>"
        "<p>Ma claim:</p><div class='code'>");
    httpd_resp_sendstr_chunk(req, claim_code);
    httpd_resp_sendstr_chunk(req, "</div><p>Mo app DeepLove -> Them thiet bi -> quet QR:</p>");
    httpd_resp_sendstr_chunk(req, s_svg);
    httpd_resp_sendstr_chunk(req, "<p class='id'>device_id:<br>");
    httpd_resp_sendstr_chunk(req, device_id);
    httpd_resp_sendstr_chunk(req, "</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /identity : JSON danh tinh cho cong cu factory dang ky backend.
static esp_err_t identity_get_handler(httpd_req_t *req)
{
    char pub_b64[80];
    b64_encode(ed_pub, sizeof(ed_pub), pub_b64, sizeof(pub_b64));

    char out[320];
    snprintf(out, sizeof(out),
             "{\"device_id\":\"%s\",\"claim_code\":\"%s\",\"public_key\":\"%s\","
             "\"setup_ap_ssid\":\"%s\",\"firmware_version\":\"%s\"}",
             device_id, claim_code, pub_b64, setup_ap_ssid, FIRMWARE_VERSION);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// GET /scan : quet WiFi xung quanh -> JSON [{"ssid","rssi"}]
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {.show_hidden = false};
    esp_wifi_scan_start(&scan_cfg, true);
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs) esp_wifi_scan_get_ap_records(&num, recs); else num = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == '\0' || strchr((char *)recs[i].ssid, '"')) continue;
        char item[96];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i ? "," : "", (char *)recs[i].ssid, recs[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(recs);
    return ESP_OK;
}

// POST /provision : app gui JSON {claim_session_id, home_wifi_ssid,
// home_wifi_password, backend_url} -> luu NVS + reboot de vao mang.
static esp_err_t provision_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[total] = '\0';

    char ssid[33] = {0}, pass[65] = {0}, sid[64] = {0}, backend[128] = {0};
    bool ok = json_str(body, "home_wifi_ssid", ssid, sizeof(ssid)) &&
              json_str(body, "claim_session_id", sid, sizeof(sid)) &&
              json_str(body, "backend_url", backend, sizeof(backend));
    json_str(body, "home_wifi_password", pass, sizeof(pass));  // co the rong
    free(body);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        return ESP_FAIL;
    }
    config_save_provision(ssid, pass, backend, sid);
    ESP_LOGW(TAG, "Da nhan provision: ssid='%s' backend='%s' -> reboot", ssid, backend);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_uri_t iden = {.uri = "/identity", .method = HTTP_GET, .handler = identity_get_handler};
        httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};
        httpd_uri_t prov = {.uri = "/provision", .method = HTTP_POST, .handler = provision_post_handler};
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &iden);
        httpd_register_uri_handler(server, &scan);
        httpd_register_uri_handler(server, &prov);
        ESP_LOGI(TAG, "Web server chay -> http://192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Khong khoi dong duoc web server");
    }
}

// ============================================================
//  SNTP (dong bo gio de ky timestamp hop le, backend cho +-5 phut)
// ============================================================
static void obtain_time(void)
{
    if (s_time_synced) return;
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
    }
    time_t now = 0;
    for (int i = 0; i < 20; i++) {
        time(&now);
        if (now > 1600000000) { s_time_synced = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "SNTP: time=%ld synced=%d", (long)now, s_time_synced);
}

// ============================================================
//  MQTT (EMQX cloud, TLS, dung creds backend cap)
// ============================================================
static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, ">>> DA KET NOI EMQX!");
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "{\"id\":\"%s\",\"status\":\"online\",\"fw\":\"%s\"}",
                 device_id, FIRMWARE_VERSION);
        if (g_mqtt_topic[0]) {
            esp_mqtt_client_publish(event->client, g_mqtt_topic, msg, 0, 1, 1);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT roi ket noi (se tu noi lai)");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT loi ket noi EMQX");
        break;
    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    if (s_mqtt != NULL || !g_mqtt_host[0] || !g_mqtt_pass[0]) {
        return;
    }
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtts://%s:%s",
             g_mqtt_host, g_mqtt_port[0] ? g_mqtt_port : "8883");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = g_mqtt_user,
        .credentials.client_id = g_mqtt_client[0] ? g_mqtt_client : device_id,
        .credentials.authentication.password = g_mqtt_pass,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "Dang noi MQTT %s (user=%s)", uri, g_mqtt_user);
}

// ============================================================
//  Hoan tat claim: HTTPS POST .../complete (ky Ed25519)
// ============================================================
#define HTTP_RESP_MAX 1024
static char s_http_resp[HTTP_RESP_MAX];
static int  s_http_resp_len;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        int n = e->data_len;
        if (s_http_resp_len + n < HTTP_RESP_MAX - 1) {
            memcpy(s_http_resp + s_http_resp_len, e->data, n);
            s_http_resp_len += n;
            s_http_resp[s_http_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t do_complete_claim(void)
{
    if (!g_backend_url[0] || !g_claim_session_id[0]) {
        return ESP_FAIL;
    }
    // nonce = 16 byte random -> hex
    uint8_t nb[16];
    char nonce[33];
    randombytes_buf(nb, sizeof(nb));
    for (int i = 0; i < 16; i++) snprintf(nonce + i * 2, 3, "%02x", nb[i]);

    long ts = (long)time(NULL);

    // message = "device_id|session_id|nonce|timestamp"
    char msg[256];
    int msglen = snprintf(msg, sizeof(msg), "%s|%s|%s|%ld",
                          device_id, g_claim_session_id, nonce, ts);

    // Ky Ed25519 -> base64
    uint8_t sig[crypto_sign_BYTES];
    unsigned long long siglen = 0;
    crypto_sign_detached(sig, &siglen, (const uint8_t *)msg, msglen, ed_priv);
    char sig_b64[128];
    if (!b64_encode(sig, (size_t)siglen, sig_b64, sizeof(sig_b64))) {
        return ESP_FAIL;
    }

    // Body JSON (snprintf: id/nonce/base64/so deu an toan JSON)
    char body[384];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"nonce\":\"%s\",\"timestamp\":%ld,"
             "\"signature\":\"%s\",\"firmware_version\":\"%s\"}",
             device_id, nonce, ts, sig_b64, FIRMWARE_VERSION);

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/device/claim-sessions/%s/complete",
             g_backend_url, g_claim_session_id);

    s_http_resp_len = 0; s_http_resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .event_handler = http_evt,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "complete: err=%s status=%d resp=%s",
             esp_err_to_name(err), status, s_http_resp);
    if (err != ESP_OK || status != 200) {
        return ESP_FAIL;
    }

    // Parse response phang -> device_token + mqtt.* (moi key la duy nhat trong resp)
    json_str(s_http_resp, "device_token", g_mqtt_pass, sizeof(g_mqtt_pass));
    json_str(s_http_resp, "host", g_mqtt_host, sizeof(g_mqtt_host));
    json_str(s_http_resp, "port", g_mqtt_port, sizeof(g_mqtt_port));
    json_str(s_http_resp, "username", g_mqtt_user, sizeof(g_mqtt_user));
    json_str(s_http_resp, "password", g_mqtt_pass, sizeof(g_mqtt_pass));
    json_str(s_http_resp, "client_id", g_mqtt_client, sizeof(g_mqtt_client));
    json_str(s_http_resp, "topic_signal", g_mqtt_topic, sizeof(g_mqtt_topic));

    if (!g_mqtt_pass[0]) {
        return ESP_FAIL;
    }
    if (!g_mqtt_user[0]) strlcpy(g_mqtt_user, device_id, sizeof(g_mqtt_user));
    config_save_mqtt();
    ESP_LOGW(TAG, ">>> CLAIM HOAN TAT! Da nhan MQTT creds tu backend.");
    return ESP_OK;
}

// Task chay sau khi co IP: dong bo gio -> claim/complete -> noi MQTT.
static void post_ip_task(void *arg)
{
    obtain_time();
    if (g_mqtt_pass[0]) {
        mqtt_app_start();                    // da claim tu truoc
    } else if (g_claim_session_id[0]) {
        if (do_complete_claim() == ESP_OK) {
            mqtt_app_start();
        } else {
            ESP_LOGE(TAG, "Hoan tat claim that bai (se thu lai lan boot sau).");
        }
    } else {
        ESP_LOGI(TAG, "Chua co phien claim -> cho app provision.");
    }
    vTaskDelete(NULL);
}

// ============================================================
//  WiFi APSTA
// ============================================================
static esp_timer_handle_t s_reconnect_timer = NULL;
#define WIFI_RECONNECT_DELAY_US (8 * 1000000)

static void wifi_reconnect_cb(void *arg)
{
    if (g_wifi_ssid[0]) esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_wifi_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_ip.addr = 0;
        if (g_wifi_ssid[0] && s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, WIFI_RECONNECT_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_sta_ip = event->ip_info.ip;
        ESP_LOGI(TAG, ">>> DA NOI WiFi '%s'! IP " IPSTR, g_wifi_ssid, IP2STR(&s_sta_ip));
        // Chay task xu ly claim/MQTT (khong block event loop).
        xTaskCreate(post_ip_task, "post_ip", 8192, NULL, 5, NULL);
    }
}

static void wifi_init_apsta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    const esp_timer_create_args_t targs = {.callback = wifi_reconnect_cb, .name = "wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(setup_ap_ssid),
            .channel = AP_CHANNEL,
            .password = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, setup_ap_ssid, sizeof(ap_config.ap.ssid));

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, g_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));

    if (g_wifi_ssid[0]) {
        ESP_LOGI(TAG, "WiFi APSTA: phat '%s' + noi WiFi nha '%s'...", setup_ap_ssid, g_wifi_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi APSTA: phat '%s' (CHUA co WiFi nha)", setup_ap_ssid);
    }
}

// ============================================================
//  MAIN
// ============================================================
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "sodium_init that bai");
    }

    device_identity_init();   // sinh/doc danh tinh + khoa Ed25519
    button_init();
    config_load();            // WiFi + backend + claim_sid + mqtt creds
    wifi_init_apsta();
    start_webserver();

    ESP_LOGI(TAG, "San sang. AP '%s' pass '%s' -> http://192.168.4.1", setup_ap_ssid, AP_PASS);

    int held_ms = 0, print_ms = 0;
    const int STEP_MS = 100;
    print_identity();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        print_ms += STEP_MS;

        if (button_pressed()) {
            held_ms += STEP_MS;
            if (held_ms % 1000 == 0) {
                int remain = (RESET_HOLD_SECONDS * 1000 - held_ms) / 1000;
                if (remain > 0) ESP_LOGW(TAG, "Giu BOOT... con %d giay se RESET", remain);
            }
            if (held_ms >= RESET_HOLD_SECONDS * 1000) {
                device_factory_reset();
                ESP_LOGW(TAG, "Da reset! THA NUT BOOT de khoi dong lai...");
                while (button_pressed()) vTaskDelay(pdMS_TO_TICKS(100));
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }

        if (print_ms >= 5000) {
            print_ms = 0;
            print_identity();
        }
    }
}
