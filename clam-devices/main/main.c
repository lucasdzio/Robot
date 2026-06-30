/*
 * CLAIM DEVICE - Bước 1 + 2 (đơn giản để hiểu)
 * -------------------------------------------------
 * Mục tiêu:
 *   - Lần đầu bật: robot TỰ SINH ra 3 thứ định danh rồi LƯU vào flash (NVS):
 *       1) device_id      : "tên" công khai, lấy từ địa chỉ MAC của chip
 *       2) device_secret  : "mật khẩu" bí mật, 16 byte ngẫu nhiên (KHÔNG in ra)
 *       3) claim_code     : "mã kích hoạt" 6 ký tự cho chủ nhận máy
 *   - Những lần bật sau: ĐỌC LẠI từ flash, KHÔNG sinh mới
 *     => reset board bao nhiêu lần thì device_id/claim_code vẫn y nguyên.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>     // atoi
#include <ctype.h>      // toupper
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"        // esp_read_mac()
#include "esp_random.h"     // esp_fill_random()
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"    // doc nut BOOT
#include "esp_wifi.h"       // phat WiFi AP
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h" // web server
#include "qrcode.h"          // sinh ma QR
#include "mqtt_client.h"     // ket noi MQTT toi EMQX

static const char *TAG = "identity";

// Tên "ngăn tủ" trong flash để cất dữ liệu định danh
#define NVS_NAMESPACE   "device"

// Nut BOOT tren board ESP32-C3 noi vao GPIO9, nhan = muc 0 (LOW).
// Giu nut nay ~3 giay luc khoi dong => factory reset (xoa danh tinh cu).
#define BOOT_BUTTON_GPIO   9
#define RESET_HOLD_SECONDS 3

// WiFi AP de hien trang claim (dien thoai noi vao day, mo http://192.168.4.1)
#define AP_SSID     "ROB-SETUP"
#define AP_PASS     "12345678"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

// WiFi nha (STA) de robot len mang, cung mang voi EMQX
// (Tam thoi gan cung trong code; buoc sau se cho nhap qua web)
#define STA_SSID    "TTTH 2"
#define STA_PASS    "Trunganhgr@123"

// Dia chi EMQX broker (may chay docker). Chua bat auth -> cho ket noi an danh.
#define MQTT_BROKER_URI "mqtt://172.16.0.52:1883"

// Bảng ký tự dùng để sinh claim_code.
// Cố tình BỎ các ký tự dễ nhìn nhầm: O/0, I/1, để người dùng đỡ gõ sai.
static const char CLAIM_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define CLAIM_CODE_LEN   6
#define DEVICE_SECRET_LEN 16

// ----- Bộ nhớ tạm để giữ giá trị sau khi đọc/sinh ra -----
static char device_id[32];                  // ví dụ: ESP32C3_A1B2C3D4E5F6
static char claim_code[CLAIM_CODE_LEN + 1]; // ví dụ: 7F92KD
static uint8_t device_secret[DEVICE_SECRET_LEN];
static uint8_t claimed = 0;                 // 0 = chưa có chủ, 1 = đã claim
static uint32_t owner_id = 0;               // ID chủ sở hữu (0 = chưa có)
static esp_ip4_addr_t s_sta_ip = { 0 };     // IP robot lấy được từ WiFi nhà (0 = chưa nối)
static esp_mqtt_client_handle_t s_mqtt = NULL; // client MQTT toi EMQX
static char s_topic_cmd[96];                 // kenh nhan lenh:   dev/<id>/cmd
static char s_topic_status[96];              // kenh bao trang thai: dev/<id>/status

// Cau hinh mang co the doi qua web (luu NVS). Mac dinh = gia tri #define o tren.
static char g_wifi_ssid[33];                 // ten WiFi nha
static char g_wifi_pass[65];                 // mat khau WiFi nha
static char g_broker[96];                    // dia chi broker, vd mqtt://172.16.0.52:1883

static void mqtt_app_start(void);            // khai bao truoc, dinh nghia o duoi

/* Doc cau hinh mang tu NVS. Neu chua co -> dung gia tri mac dinh (#define). */
static void config_load(void)
{
    // Mac dinh
    strlcpy(g_wifi_ssid, STA_SSID, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, STA_PASS, sizeof(g_wifi_pass));
    strlcpy(g_broker,    MQTT_BROKER_URI, sizeof(g_broker));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(g_wifi_ssid); nvs_get_str(nvs, "wifi_ssid", g_wifi_ssid, &len);
        len = sizeof(g_wifi_pass); nvs_get_str(nvs, "wifi_pass", g_wifi_pass, &len);
        len = sizeof(g_broker);    nvs_get_str(nvs, "broker",    g_broker,    &len);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Config: WiFi='%s'  broker='%s'", g_wifi_ssid, g_broker);
}

/* Luu cau hinh mang xuong NVS (chi luu truong co nhap) */
static void config_save(const char *ssid, const char *pass, const char *broker)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (ssid   && ssid[0])   nvs_set_str(nvs, "wifi_ssid", ssid);
        if (pass   && pass[0])   nvs_set_str(nvs, "wifi_pass", pass);
        if (broker && broker[0]) nvs_set_str(nvs, "broker",    broker);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Sinh device_id từ địa chỉ MAC của chip (mỗi chip 1 MAC duy nhất) */
static void generate_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id),
             "ESP32C3_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Sinh claim_code: 6 ký tự ngẫu nhiên lấy từ CLAIM_ALPHABET */
static void generate_claim_code(void)
{
    int alphabet_len = sizeof(CLAIM_ALPHABET) - 1; // trừ ký tự '\0'
    for (int i = 0; i < CLAIM_CODE_LEN; i++) {
        uint32_t r = esp_random();             // số ngẫu nhiên 32-bit
        claim_code[i] = CLAIM_ALPHABET[r % alphabet_len];
    }
    claim_code[CLAIM_CODE_LEN] = '\0';
}

/* Sinh device_secret: 16 byte ngẫu nhiên thật (dùng để ký HMAC sau này) */
static void generate_device_secret(void)
{
    esp_fill_random(device_secret, DEVICE_SECRET_LEN);
}

/* Sinh mới toàn bộ danh tính rồi lưu xuống flash */
static void create_and_save_identity(nvs_handle_t nvs)
{
    ESP_LOGW(TAG, "Lan dau khoi dong -> dang sinh danh tinh moi...");

    generate_device_id();
    generate_claim_code();
    generate_device_secret();
    claimed = 0;
    owner_id = 0;

    // Lưu vào flash. Mỗi giá trị là 1 "key" trong ngăn tủ "device".
    ESP_ERROR_CHECK(nvs_set_str(nvs, "device_id", device_id));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "claim_code", claim_code));
    ESP_ERROR_CHECK(nvs_set_blob(nvs, "secret", device_secret, DEVICE_SECRET_LEN));
    ESP_ERROR_CHECK(nvs_set_u8(nvs, "claimed", claimed));
    ESP_ERROR_CHECK(nvs_set_u32(nvs, "owner", owner_id));
    ESP_ERROR_CHECK(nvs_commit(nvs)); // ghi thật xuống flash
}

/* Đọc lại danh tính đã lưu từ flash */
static void load_identity(nvs_handle_t nvs)
{
    size_t len;

    len = sizeof(device_id);
    ESP_ERROR_CHECK(nvs_get_str(nvs, "device_id", device_id, &len));

    len = sizeof(claim_code);
    ESP_ERROR_CHECK(nvs_get_str(nvs, "claim_code", claim_code, &len));

    len = DEVICE_SECRET_LEN;
    ESP_ERROR_CHECK(nvs_get_blob(nvs, "secret", device_secret, &len));

    ESP_ERROR_CHECK(nvs_get_u8(nvs, "claimed", &claimed));

    // owner co the chua ton tai (firmware cu) -> mac dinh 0, khong bao loi
    if (nvs_get_u32(nvs, "owner", &owner_id) != ESP_OK) {
        owner_id = 0;
    }
}

/*
 * FACTORY RESET: xoa toan bo danh tinh trong NVS.
 * Luu y: device_id se KHONG mat that su, vi no lay lai tu MAC o lan boot sau.
 * Nhung claim_code + secret se duoc SINH MOI, va claimed ve 0 (chua co chu).
 */
static void device_factory_reset(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);   // xoa het key trong ngan tu "device"
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "*** FACTORY RESET *** Da xoa danh tinh cu -> se sinh claim_code/secret moi.");
}

/* Ket qua cua mot lan thu claim */
typedef enum {
    CLAIM_OK,        // claim thanh cong
    CLAIM_ALREADY,   // thiet bi da co chu roi
    CLAIM_BADCODE,   // sai claim_code
    CLAIM_BADINPUT,  // thieu thong tin
} claim_result_t;

/*
 * XU LY CLAIM (tam thoi robot dong vai server):
 *   - Kiem tra da co chu chua
 *   - Kiem tra claim_code co dung khong
 *   - Neu ok: ghi claimed=1 + owner_id vao NVS
 * Sau nay buoc 7 se thay phan nay bang "goi server that".
 */
static claim_result_t device_do_claim(uint32_t uid, const char *code)
{
    if (uid == 0 || code == NULL || code[0] == '\0') {
        return CLAIM_BADINPUT;
    }
    if (claimed) {
        return CLAIM_ALREADY;       // da co chu -> khong cho claim lai
    }
    if (strcmp(code, claim_code) != 0) {
        return CLAIM_BADCODE;       // sai ma
    }

    // Thanh cong -> ghi chu so huu
    claimed = 1;
    owner_id = uid;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "claimed", claimed);
        nvs_set_u32(nvs, "owner", owner_id);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, ">>> CLAIM thanh cong! Thiet bi gio thuoc ve user %lu", (unsigned long)owner_id);
    return CLAIM_OK;
}

/* Cau hinh nut BOOT (GPIO9) la input co dien tro keo len.
 * Nha nut = HIGH (1), nhan nut = LOW (0). */
static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

/* Tra ve true neu nut BOOT dang duoc nhan (muc LOW) */
static bool button_pressed(void)
{
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

/* Khởi tạo danh tính: lần đầu thì sinh mới, các lần sau thì đọc lại */
static void device_identity_init(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));

    // Thử đọc device_id. Nếu CHƯA CÓ -> đây là lần đầu -> sinh mới.
    size_t len = sizeof(device_id);
    esp_err_t err = nvs_get_str(nvs, "device_id", device_id, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        create_and_save_identity(nvs);   // lần đầu
    } else {
        ESP_ERROR_CHECK(err);
        load_identity(nvs);              // các lần sau
        ESP_LOGI(TAG, "Da co danh tinh trong flash -> doc lai.");
    }

    nvs_close(nvs);
}

/* In ra màn hình (CHÚ Ý: không in device_secret để giữ bí mật) */
static void print_identity(void)
{
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "        DEVICE IDENTITY");
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, " device_id  : %s", device_id);
    ESP_LOGI(TAG, " claim_code : %s", claim_code);
    ESP_LOGI(TAG, " secret     : (da luu, an vi bao mat)");
    if (claimed) {
        ESP_LOGI(TAG, " claimed    : DA co chu (owner=%lu)", (unsigned long)owner_id);
    } else {
        ESP_LOGI(TAG, " claimed    : CHUA co chu");
    }
    ESP_LOGI(TAG, " WiFi AP    : %s  -> http://192.168.4.1", AP_SSID);
    if (s_sta_ip.addr != 0) {
        ESP_LOGI(TAG, " WiFi nha   : %s  IP " IPSTR, STA_SSID, IP2STR(&s_sta_ip));
    } else {
        ESP_LOGI(TAG, " WiFi nha   : %s  (dang noi...)", STA_SSID);
    }
    ESP_LOGI(TAG, "===================================");
}

// ===================== PHAN WEB + QR =====================

// Bo nho de chua chuoi SVG (hinh QR ve bang cac o vuong)
static char s_svg[32768];
static size_t s_svg_len;

// Noi them chuoi vao s_svg mot cach an toan (khong tran bo nho)
static void svg_append(const char *s)
{
    size_t n = strlen(s);
    if (s_svg_len + n < sizeof(s_svg) - 1) {
        memcpy(s_svg + s_svg_len, s, n);
        s_svg_len += n;
        s_svg[s_svg_len] = '\0';
    }
}

// Ham nay duoc thu vien QR goi sau khi ma hoa xong.
// Ta doc tung o (module) cua QR roi ve thanh cac hinh vuong den trong SVG.
static void qr_to_svg(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);   // so o moi canh (vd 29)
    const int scale = 8;                  // moi o ve to 8x8 pixel
    const int quiet = 4;                  // vien trang quanh QR (chuan QR)
    int dim = (size + quiet * 2) * scale;

    char tmp[160];
    snprintf(tmp, sizeof(tmp),
             "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' shape-rendering='crispEdges'>",
             dim, dim);
    svg_append(tmp);
    svg_append("<rect width='100%' height='100%' fill='white'/>");

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {   // o nay mau den
                snprintf(tmp, sizeof(tmp),
                         "<rect x='%d' y='%d' width='%d' height='%d' fill='black'/>",
                         (x + quiet) * scale, (y + quiet) * scale, scale, scale);
                svg_append(tmp);
            }
        }
    }
    svg_append("</svg>");
}

// Sinh QR tu chuoi text -> ket qua nam trong s_svg
static void build_qr_svg(const char *text)
{
    s_svg_len = 0;
    s_svg[0] = '\0';
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_to_svg;            // bao thu vien goi ham ve SVG cua ta
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, text);
}

// Xu ly khi dien thoai mo trang "/" -> tra ve HTML co claim_code + QR
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // Noi dung nhet vao QR: device_id + claim_code (dang JSON gon)
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":\"%s\"}", device_id, claim_code);
    build_qr_svg(payload);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Claim Device</title><style>"
        "body{font-family:sans-serif;text-align:center;background:#f0f2f5;margin:0;padding:24px}"
        ".card{background:#fff;max-width:360px;margin:auto;padding:24px;border-radius:16px;box-shadow:0 4px 12px #0002}"
        "h2{margin:0 0 16px}.code{font-size:32px;font-weight:bold;letter-spacing:4px;color:#1565c0;margin:8px 0}"
        ".id{font-size:13px;color:#666;word-break:break-all}.tag{display:inline-block;padding:4px 10px;border-radius:12px;font-size:13px}"
        "</style></head><body><div class='card'>"
        "<h2>🤖 Claim Device</h2>");

    // Trang thai claimed
    httpd_resp_sendstr_chunk(req, claimed
        ? "<div class='tag' style='background:#e8f5e9;color:#2e7d32'>DA co chu</div>"
        : "<div class='tag' style='background:#fff3e0;color:#e65100'>CHUA co chu</div>");

    // Ma claim
    httpd_resp_sendstr_chunk(req, "<p>Ma claim cua thiet bi:</p><div class='code'>");
    httpd_resp_sendstr_chunk(req, claim_code);
    httpd_resp_sendstr_chunk(req, "</div>");

    // Hinh QR
    httpd_resp_sendstr_chunk(req, "<p>Quet QR de them thiet bi:</p>");
    httpd_resp_sendstr_chunk(req, s_svg);

    // ----- Phan CLAIM -----
    if (claimed) {
        // Da co chu -> hien owner
        char owner_buf[80];
        snprintf(owner_buf, sizeof(owner_buf),
                 "<p style='color:#2e7d32;font-weight:bold'>✅ Da claim boi user %lu</p>",
                 (unsigned long)owner_id);
        httpd_resp_sendstr_chunk(req, owner_buf);
    } else {
        // Chua co chu -> hien form de claim thu
        httpd_resp_sendstr_chunk(req,
            "<hr><p><b>Thu claim thiet bi:</b></p>"
            "<form action='/claim' method='get'>"
            "<input name='uid' type='number' placeholder='User ID (vd 123)' "
            "style='width:90%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:8px'><br>"
            "<input name='code' placeholder='Nhap claim code' "
            "style='width:90%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:8px'><br>"
            "<button type='submit' "
            "style='padding:10px 24px;margin-top:8px;background:#1565c0;color:#fff;border:0;border-radius:8px;font-size:16px'>"
            "Claim</button></form>");
    }

    // ----- Phan CAU HINH MANG (WiFi + broker) -----
    httpd_resp_sendstr_chunk(req,
        "<hr><p><b>Cau hinh mang:</b></p>"
        "<form action='/config' method='get'>"
        "<select name='ssid' id='ssid' "
        "style='width:92%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:8px'>"
        "<option value=''>-- Bam 'Quet WiFi' --</option></select><br>"
        "<button type='button' onclick='scanWifi()' "
        "style='padding:6px 16px;margin:4px;background:#eee;border:1px solid #ccc;border-radius:8px'>"
        "🔄 Quet WiFi</button><br>"
        "<input name='pass' type='password' placeholder='Mat khau WiFi' "
        "style='width:90%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:8px'><br>"
        "<input name='broker' placeholder='mqtt://172.16.0.52:1883' value='");
    httpd_resp_sendstr_chunk(req, g_broker);
    httpd_resp_sendstr_chunk(req,
        "' style='width:90%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:8px'><br>"
        "<button type='submit' "
        "style='padding:10px 24px;margin-top:8px;background:#2e7d32;color:#fff;border:0;border-radius:8px;font-size:16px'>"
        "Luu &amp; khoi dong lai</button></form>"
        // JS goi /scan roi do vao dropdown
        "<script>"
        "function scanWifi(){"
        "var s=document.getElementById('ssid');"
        "s.innerHTML='<option>Dang quet...</option>';"
        "fetch('/scan').then(r=>r.json()).then(function(list){"
        "s.innerHTML='';"
        "list.sort((a,b)=>b.rssi-a.rssi);"
        "list.forEach(function(w){var o=document.createElement('option');"
        "o.value=w.ssid;o.text=w.ssid+'  ('+w.rssi+'dBm)';s.add(o);});"
        "if(!list.length){s.innerHTML='<option>Khong thay WiFi nao</option>';}"
        "}).catch(e=>{s.innerHTML='<option>Loi quet</option>';});"
        "}"
        "</script>");

    // device_id
    httpd_resp_sendstr_chunk(req, "<p class='id'>device_id:<br>");
    httpd_resp_sendstr_chunk(req, device_id);
    httpd_resp_sendstr_chunk(req, "</p></div></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);  // ket thuc
    return ESP_OK;
}

// Xu ly khi nguoi dung bam nut "Claim" tren web (/claim?uid=...&code=...)
static esp_err_t claim_get_handler(httpd_req_t *req)
{
    char query[160] = {0};
    char uid_s[16] = {0};
    char code_s[16] = {0};

    // Lay tham so tu duong dan ?uid=...&code=...
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "uid", uid_s, sizeof(uid_s));
        httpd_query_key_value(query, "code", code_s, sizeof(code_s));
    }

    // Doi claim_code nhap vao thanh CHU HOA cho khoi nham hoa thuong
    for (int i = 0; code_s[i]; i++) {
        code_s[i] = toupper((unsigned char)code_s[i]);
    }

    uint32_t uid = (uint32_t)atoi(uid_s);
    claim_result_t r = device_do_claim(uid, code_s);

    // Tra ve trang ket qua
    const char *msg, *color;
    switch (r) {
        case CLAIM_OK:       msg = "✅ Claim thanh cong! Thiet bi gio la cua ban."; color = "#2e7d32"; break;
        case CLAIM_ALREADY:  msg = "⚠️ Thiet bi nay da co chu roi.";                color = "#e65100"; break;
        case CLAIM_BADCODE:  msg = "❌ Sai claim code. Kiem tra lai.";              color = "#c62828"; break;
        default:             msg = "❌ Thieu thong tin (User ID hoac code).";        color = "#c62828"; break;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#f0f2f5}"
        ".card{background:#fff;max-width:340px;margin:auto;padding:24px;border-radius:16px;box-shadow:0 4px 12px #0002}"
        "a{display:inline-block;margin-top:16px;color:#1565c0}</style></head><body><div class='card'>"
        "<h3 style='color:");
    httpd_resp_sendstr_chunk(req, color);
    httpd_resp_sendstr_chunk(req, "'>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</h3><a href='/'>&larr; Quay lai</a></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Giai ma chuoi tu URL: doi %40 -> '@', '+' -> ' ' ... (sua truc tiep tren s)
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// /scan : quet WiFi xung quanh, tra ve danh sach dang JSON [{"ssid":..,"rssi":..},...]
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);     // true = cho quet xong

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;                    // gioi han 20 mang
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs) {
        esp_wifi_scan_get_ap_records(&num, recs);
    } else {
        num = 0;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < num; i++) {
        // bo qua SSID rong hoac chua dau " (tranh hong JSON)
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

// /config : luu WiFi + broker do nguoi dung chon, roi khoi dong lai de ap dung
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char ssid[33] = {0}, pass[65] = {0}, broker[96] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "ssid",   ssid,   sizeof(ssid));
        httpd_query_key_value(query, "pass",   pass,   sizeof(pass));
        httpd_query_key_value(query, "broker", broker, sizeof(broker));
        url_decode(ssid); url_decode(pass); url_decode(broker);
    }
    config_save(ssid, pass, broker);
    ESP_LOGW(TAG, "Da luu cau hinh moi: WiFi='%s' broker='%s' -> khoi dong lai", ssid, broker);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#f0f2f5}</style>"
        "</head><body><h3>✅ Da luu cau hinh!</h3>"
        "<p>Robot dang khoi dong lai de ket noi WiFi moi...</p>"
        "<p>Doi ~10 giay roi vao lai <a href='http://192.168.4.1'>192.168.4.1</a></p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);

    // Cho gui xong response roi reboot
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// Khoi dong web server, dang ky cac trang
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root  = { .uri = "/",       .method = HTTP_GET, .handler = root_get_handler };
        httpd_uri_t claim = { .uri = "/claim",  .method = HTTP_GET, .handler = claim_get_handler };
        httpd_uri_t scan  = { .uri = "/scan",   .method = HTTP_GET, .handler = scan_get_handler };
        httpd_uri_t cfg   = { .uri = "/config", .method = HTTP_GET, .handler = config_get_handler };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &claim);
        httpd_register_uri_handler(server, &scan);
        httpd_register_uri_handler(server, &cfg);
        ESP_LOGI(TAG, "Web server da chay -> http://192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Khong khoi dong duoc web server");
    }
}

// Xu ly su kien WiFi: khi nao nen ket noi, mat ket noi, lay duoc IP...
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();                          // bat dau noi WiFi nha
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_sta_ip.addr = 0;
        ESP_LOGW(TAG, "WiFi nha roi ket noi (ssid='%s' reason=%d) -> thu noi lai...",
                 g_wifi_ssid, d ? d->reason : -1);
        esp_wifi_connect();                          // tu dong thu lai
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_sta_ip = event->ip_info.ip;
        ESP_LOGI(TAG, ">>> DA NOI WiFi nha '%s'! IP cua robot: " IPSTR, STA_SSID, IP2STR(&s_sta_ip));
        // Co mang roi -> ket noi MQTT toi EMQX (chi khoi dong 1 lan)
        if (s_mqtt == NULL) {
            mqtt_app_start();
        }
    }
}

// Che do APSTA: vua phat AP (ROB-SETUP) vua noi WiFi nha (TTTH)
static void wifi_init_apsta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Dang ky ham xu ly su kien WiFi + IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Cau hinh AP (cho trang claim)
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .password = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // Cau hinh STA (noi WiFi nha) - lay tu cau hinh runtime (co the doi qua web)
    wifi_config_t sta_config = { 0 };
    strlcpy((char *)sta_config.sta.ssid,     g_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));   // cong suat toi da

    ESP_LOGI(TAG, "WiFi APSTA: phat '%s' + dang noi vao WiFi nha '%s'...", AP_SSID, g_wifi_ssid);
}

// ===================== PHAN MQTT (ket noi EMQX) =====================

// Xu ly su kien MQTT: ket noi, mat ket noi, nhan tin, loi...
static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, ">>> DA KET NOI EMQX! Sub kenh lenh: %s", s_topic_cmd);
        esp_mqtt_client_subscribe(event->client, s_topic_cmd, 0);

        // Bao trang thai online (giu lai - retain) kem device_id, claimed, owner
        char msg[180];
        snprintf(msg, sizeof(msg),
                 "{\"id\":\"%s\",\"status\":\"online\",\"claimed\":%d,\"owner\":%lu}",
                 device_id, claimed, (unsigned long)owner_id);
        esp_mqtt_client_publish(event->client, s_topic_status, msg, 0, 1, 1);
        ESP_LOGI(TAG, "Da bao trang thai len kenh: %s", s_topic_status);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT roi ket noi EMQX (se tu noi lai)");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Nhan lenh: [%.*s] = %.*s",
                 event->topic_len, event->topic, event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT loi! Co the robot khong toi duoc broker %s", MQTT_BROKER_URI);
        break;
    default:
        break;
    }
}

// Khoi tao va bat dau client MQTT
static void mqtt_app_start(void)
{
    // Tao ten kenh rieng theo device_id: dev/<id>/cmd va dev/<id>/status
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "dev/%s/cmd", device_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "dev/%s/status", device_id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = g_broker,       // dia chi broker (cau hinh qua web)
        .credentials.client_id = device_id,   // dung device_id lam Client ID
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "Dang ket noi MQTT toi %s ...", g_broker);
}

void app_main(void)
{
    // 1) Khởi tạo NVS (vùng flash để lưu cấu hình) - giống các project cũ
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2) Tạo hoặc đọc danh tính thiết bị
    device_identity_init();

    // 3) Cau hinh nut BOOT de dung lam nut factory reset luc app dang chay
    button_init();

    // 3a) Doc cau hinh mang (WiFi + broker) tu flash, neu chua co thi dung mac dinh
    config_load();

    // 3b) Phat WiFi AP + noi WiFi nha (APSTA) + mo web server
    wifi_init_apsta();
    start_webserver();

    ESP_LOGI(TAG, "Dien thoai noi WiFi '%s' (mat khau %s) roi mo http://192.168.4.1", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Dang chay. GIU nut BOOT %d giay (khi app da chay) de FACTORY RESET.", RESET_HOLD_SECONDS);

    // 4) Vong lap chinh:
    //    - In danh tinh moi 3 giay
    //    - Theo doi nut BOOT: giu du RESET_HOLD_SECONDS giay -> reset roi khoi dong lai
    int held_ms = 0;       // da giu nut bao lau (ms)
    int print_ms = 0;      // dem de in moi 3 giay
    const int STEP_MS = 100;

    print_identity();      // in ngay 1 lan luc bat dau
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        print_ms += STEP_MS;

        // ----- Theo doi nut BOOT -----
        if (button_pressed()) {
            held_ms += STEP_MS;
            // Moi khi qua 1 giay tron thi bao con lai bao nhieu
            if (held_ms % 1000 == 0) {
                int remain = (RESET_HOLD_SECONDS * 1000 - held_ms) / 1000;
                if (remain > 0) {
                    ESP_LOGW(TAG, "Dang giu BOOT... con %d giay nua se RESET (tha nut de huy)", remain);
                }
            }
            if (held_ms >= RESET_HOLD_SECONDS * 1000) {
                device_factory_reset();
                // Doi tha nut roi moi reboot, neu khong chip se vao download mode
                ESP_LOGW(TAG, "Da reset! HAY THA NUT BOOT ra de khoi dong lai...");
                while (button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                ESP_LOGW(TAG, "Khoi dong lai de ap dung danh tinh moi...");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();      // reboot -> device_identity_init() sinh moi
            }
        } else {
            if (held_ms > 0) {
                ESP_LOGI(TAG, "Da tha nut -> huy reset.");
            }
            held_ms = 0;            // tha nut thi dem lai tu dau
        }

        // ----- In danh tinh moi 3 giay -----
        if (print_ms >= 3000) {
            print_ms = 0;
            print_identity();
        }
    }
}
