#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#define AP_SSID          "THUC"
#define AP_PASS          "12345678"

static const char *TAG = "wifi_analyzer";
static wifi_ap_record_t s_scanned_records[30];
static int s_scanned_count = 0;
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;

// HTML page for Wi-Fi Analyzer Dashboard (100% Offline SVG Chart)
static const char *ANALYZER_HTML = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
"<title>ESP32-C3 WiFi Analyzer</title>"
"<style>"
"body{margin:0;padding:0;background:#0d1117;color:#c9d1d9;font-family:'Segoe UI',sans-serif;display:flex;flex-direction:column;align-items:center;min-height:100vh}"
".container{width:90%;max-width:900px;margin:30px auto;padding:20px;background:#161b22;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.5);border:1px solid #30363d}"
"h1{margin:0 0 10px;font-size:26px;color:#58a6ff;text-align:center;text-shadow:0 0 10px rgba(88,166,255,0.4)}"
".radar-container{display:flex;justify-content:center;align-items:center;margin:15px 0}"
".radar{width:50px;height:50px;border-radius:50%;border:2px solid #58a6ff;position:relative;animation:pulsate 1.5s infinite ease-in-out}"
".radar::after{content:'';width:10px;height:10px;background:#58a6ff;border-radius:50%;position:absolute;top:20px;left:20px}"
"@keyframes pulsate{0%{transform:scale(0.8);opacity:0.5}50%{transform:scale(1.1);opacity:1;box-shadow:0 0 15px rgba(88,166,255,0.7)}100%{transform:scale(0.8);opacity:0.5}}"
".chart-box{background:#0d1117;border-radius:12px;border:1px solid #30363d;padding:15px;margin-bottom:20px;position:relative}"
".chart-title{font-size:16px;color:#8b949e;margin-bottom:10px;font-weight:600}"
"svg{width:100%;height:220px;display:block}"
".list-box{background:#0d1117;border-radius:12px;border:1px solid #30363d;padding:15px}"
".ap-row{display:flex;justify-content:space-between;align-items:center;padding:12px;border-bottom:1px solid #21262d;transition:background 0.2s}"
".ap-row:last-child{border-bottom:none}"
".ap-row:hover{background:#161b22}"
".ap-info{flex:2}"
".ap-ssid{font-weight:bold;color:#f0f6fc;font-size:16px}"
".ap-meta{font-size:12px;color:#8b949e;margin-top:4px}"
".ap-signal{flex:1;text-align:right}"
".signal-badge{display:inline-block;padding:4px 8px;border-radius:12px;font-size:13px;font-weight:bold}"
".sig-excellent{background:rgba(57,255,20,0.15);color:#39ff14;border:1px solid #39ff14}"
".sig-good{background:rgba(255,215,0,0.15);color:#ffd700;border:1px solid #ffd700}"
".sig-fair{background:rgba(255,140,0,0.15);color:#ff8c00;border:1px solid #ff8c00}"
".sig-weak{background:rgba(255,7,58,0.15);color:#ff073a;border:1px solid #ff073a}"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<h1>📶 ESP32-C3 Wi-Fi Analyzer 📶</h1>"
"<div class=\"radar-container\">"
"<div class=\"radar\"></div>"
"</div>"
"<div class=\"chart-box\">"
"<div class=\"chart-title\">Pho tin hieu 2.4 GHz (Kenh 1 - 13)</div>"
"<svg id=\"spectrumSvg\" viewBox=\"0 0 800 220\"></svg>"
"</div>"
"<div class=\"list-box\">"
"<div class=\"chart-title\">Danh sach mang Wi-Fi lan can</div>"
"<div id=\"apList\"></div>"
"</div>"
"</div>"
"<script>"
"const spectrumSvg=document.getElementById('spectrumSvg');"
"const apList=document.getElementById('apList');"
"function getXForChannel(chan){"
"const padding=50;const width=800;"
"return padding+(chan-1)*(width-2*padding)/12;"
"}"
"function getSignalColor(rssi){"
"if(rssi>=-60)return '#39ff14';"
"if(rssi>=-70)return '#ffd700';"
"if(rssi>=-85)return '#ff8c00';"
"return '#ff073a';"
"}"
"function getSignalClass(rssi){"
"if(rssi>=-60)return 'sig-excellent';"
"if(rssi>=-70)return 'sig-good';"
"if(rssi>=-85)return 'sig-fair';"
"return 'sig-weak';"
"}"
"function getSignalLabel(rssi){"
"if(rssi>=-60)return 'Manh';"
"if(rssi>=-70)return 'Kha';"
"if(rssi>=-85)return 'Trung binh';"
"return 'Yeu';"
"}"
"function updateUI(data){"
"spectrumSvg.innerHTML='';"
"const y_bottom=180;"
"for(let chan=1;chan<=13;chan++){"
"const x=getXForChannel(chan);"
"const line=document.createElementNS('http://www.w3.org/2000/svg','line');"
"line.setAttribute('x1',x);line.setAttribute('y1',20);line.setAttribute('x2',x);line.setAttribute('y2',y_bottom);"
"line.setAttribute('stroke','#21262d');line.setAttribute('stroke-dasharray','4 4');"
"spectrumSvg.appendChild(line);"
"const txt=document.createElementNS('http://www.w3.org/2000/svg','text');"
"txt.setAttribute('x',x);txt.setAttribute('y',y_bottom+20);txt.setAttribute('fill','#8b949e');"
"txt.setAttribute('font-size','12');txt.setAttribute('text-anchor','middle');"
"txt.textContent='CH '+chan;"
"spectrumSvg.appendChild(txt);"
"}"
"const ground=document.createElementNS('http://www.w3.org/2000/svg','line');"
"ground.setAttribute('x1',0);ground.setAttribute('y1',y_bottom);ground.setAttribute('x2',800);ground.setAttribute('y2',y_bottom);"
"ground.setAttribute('stroke','#30363d');ground.setAttribute('stroke-width','2');"
"spectrumSvg.appendChild(ground);"
"data.forEach(ap=>{"
"const chan=ap.channel;const rssi=ap.rssi;"
"if(chan<1||chan>13)return;"
"const x_peak=getXForChannel(chan);"
"const signalPercent=Math.max(5,Math.min(100,(rssi+100)*1.4));"
"const curveHeight=signalPercent*1.5;"
"const y_peak=y_bottom-curveHeight;"
"const x_start=getXForChannel(chan-2);"
"const x_end=getXForChannel(chan+2);"
"const color=getSignalColor(rssi);"
"const path=document.createElementNS('http://www.w3.org/2000/svg','path');"
"const d=`M ${x_start} ${y_bottom} Q ${x_peak} ${y_peak} ${x_end} ${y_bottom}`;"
"path.setAttribute('d',d);path.setAttribute('fill',color+'18');path.setAttribute('stroke',color);"
"path.setAttribute('stroke-width','2');"
"spectrumSvg.appendChild(path);"
"const label=document.createElementNS('http://www.w3.org/2000/svg','text');"
"label.setAttribute('x',x_peak);label.setAttribute('y',y_peak-5);label.setAttribute('fill','#c9d1d9');"
"label.setAttribute('font-size','10');label.setAttribute('text-anchor','middle');"
"label.textContent=ap.ssid||'Hidden';"
"spectrumSvg.appendChild(label);"
"});"
"apList.innerHTML='';"
"if(data.length===0){"
"apList.innerHTML='<div style=\"text-align:center;padding:20px;color:#8b949e\">Dang tim kiem tin hieu...</div>';"
"return;"
"}"
"data.sort((a,b)=>b.rssi-a.rssi);"
"data.forEach(ap=>{"
"const row=document.createElement('div');row.className='ap-row';"
"const info=document.createElement('div');info.className='ap-info';"
"const ssid=document.createElement('div');ssid.className='ap-ssid';"
"ssid.textContent=ap.ssid||'[Hidden Network]';"
"const meta=document.createElement('div');meta.className='ap-meta';"
"meta.textContent=`Kenh: ${ap.channel} | Bao mat: ${ap.auth} | Cuong do: ${ap.rssi} dBm`;"
"info.appendChild(ssid);info.appendChild(meta);"
"const signal=document.createElement('div');signal.className='ap-signal';"
"const badge=document.createElement('span');"
"badge.className=`signal-badge ${getSignalClass(ap.rssi)}`;"
"badge.textContent=`${getSignalLabel(ap.rssi)} (${ap.rssi} dBm)`;"
"signal.appendChild(badge);row.appendChild(info);row.appendChild(signal);"
"apList.appendChild(row);"
"});"
"}"
"function scan(){"
"fetch('/scan')"
".then(res=>res.json())"
".then(data=>updateUI(data))"
".catch(err=>console.error('Scan failed',err));"
"}"
"scan();"
"setInterval(scan,3000);"
"</script>"
"</body>"
"</html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ANALYZER_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    httpd_resp_send_chunk(req, "[", 1);

    bool first = true;
    wifi_ap_record_t *temp_records = malloc(sizeof(wifi_ap_record_t) * 30);
    if (temp_records == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_scan_lock);
    int count = s_scanned_count;
    if (count > 30) count = 30;
    memcpy(temp_records, s_scanned_records, sizeof(wifi_ap_record_t) * count);
    portEXIT_CRITICAL(&s_scan_lock);

    for (int i = 0; i < count; i++) {
        char item[256];
        char ssid[33];
        memset(ssid, 0, sizeof(ssid));
        memcpy(ssid, temp_records[i].ssid, sizeof(temp_records[i].ssid));
        
        // Sanitize SSID strings
        for (int j = 0; ssid[j] != '\0'; j++) {
            if (ssid[j] == '"' || ssid[j] == '\\') {
                ssid[j] = ' ';
            }
        }

        const char *auth_mode = "Unknown";
        switch (temp_records[i].authmode) {
            case WIFI_AUTH_OPEN: auth_mode = "Open"; break;
            case WIFI_AUTH_WEP: auth_mode = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_mode = "WPA-PSK"; break;
            case WIFI_AUTH_WPA2_PSK: auth_mode = "WPA2-PSK"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_mode = "WPA/WPA2-PSK"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_mode = "WPA2-Ent"; break;
            case WIFI_AUTH_WPA3_PSK: auth_mode = "WPA3-PSK"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_mode = "WPA2/WPA3-PSK"; break;
            default: break;
        }

        snprintf(item, sizeof(item), 
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\"}",
                 first ? "" : ",",
                 ssid,
                 temp_records[i].rssi,
                 temp_records[i].primary,
                 auth_mode);
        
        first = false;
        httpd_resp_send_chunk(req, item, strlen(item));
    }

    free(temp_records);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };

        httpd_uri_t scan = {
            .uri       = "/scan",
            .method    = HTTP_GET,
            .handler   = scan_get_handler,
            .user_ctx  = NULL
        };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &scan);
        return server;
    }

    ESP_LOGE(TAG, "Failed to start web server!");
    return NULL;
}

static void wifi_init_apsta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen(AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "softAP configured. AP SSID:%s", AP_SSID);
}

static void scan_task(void *arg)
{
    // Wait for WiFi system to stabilize before scanning
    vTaskDelay(pdMS_TO_TICKS(12000));
    
    while (1) {
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        
        ESP_LOGI(TAG, "Starting background WiFi scan...");
        esp_err_t err = esp_wifi_scan_start(&scan_config, true);
        if (err == ESP_OK) {
            uint16_t ap_num = 30;
            wifi_ap_record_t ap_records[30];
            
            // Get records
            if (esp_wifi_scan_get_ap_records(&ap_num, ap_records) == ESP_OK) {
                portENTER_CRITICAL(&s_scan_lock);
                s_scanned_count = ap_num;
                memcpy(s_scanned_records, ap_records, sizeof(wifi_ap_record_t) * ap_num);
                portEXIT_CRITICAL(&s_scan_lock);
                ESP_LOGI(TAG, "Scan finished. Found %d APs", ap_num);
            }
        } else {
            ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        }
        
        // Wait 6 seconds before next scan
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting WiFi Analyzer on ESP32-C3...");
    wifi_init_apsta();
    start_webserver();

    xTaskCreate(scan_task, "scan_task", 4096, NULL, 4, NULL);
}
