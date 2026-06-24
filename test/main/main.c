#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"

// Wi-Fi Configuration
#define WIFI_SSID      "TTTH"
#define WIFI_PASS      "Trunganhgr@123"
#define WIFI_MAX_RETRY 5

// MQTT Configuration
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define MQTT_SUB_TOPIC  "robot/control/letien"
#define MQTT_PUB_TOPIC  "robot/status/letien"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "mqtt_ai_test";
static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static int s_retry_num = 0;

// AI Variables
static bool s_inject_anomaly = false;
static bool s_mqtt_connected = false;

// Mock Motor Controls
static void both_motors_turn_left(void)
{
    ESP_LOGI(TAG, "[Simulation] Motors turning LEFT");
}

static void both_motors_turn_right(void)
{
    ESP_LOGI(TAG, "[Simulation] Motors turning RIGHT");
}

static void both_motors_stop(void)
{
    ESP_LOGI(TAG, "[Simulation] Motors STOPPED");
}

// ==========================================
// TINY AI: Autoencoder Inference Engine (C)
// ==========================================
#define INPUT_SIZE 10
#define HIDDEN_SIZE 4

// Math definitions
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// We handcraft the weights of the Autoencoder.
// It acts as a projection onto the first two Fourier harmonics (sine & cosine components).
// w1[10][4] projects the input vector onto the normal frequencies.
static float w1[INPUT_SIZE][HIDDEN_SIZE];
static float w2[HIDDEN_SIZE][INPUT_SIZE];

static void init_autoencoder_weights(void)
{
    for (int i = 0; i < INPUT_SIZE; i++) {
        // Fundamental harmonic (50Hz component)
        w1[i][0] = sin(2.0 * M_PI * i / INPUT_SIZE);
        w1[i][1] = cos(2.0 * M_PI * i / INPUT_SIZE);
        
        // Second harmonic (100Hz component)
        w1[i][2] = sin(4.0 * M_PI * i / INPUT_SIZE);
        w1[i][3] = cos(4.0 * M_PI * i / INPUT_SIZE);
    }
    
    // Reconstruction weights (transpose scaled by 2/N)
    for (int j = 0; j < HIDDEN_SIZE; j++) {
        for (int i = 0; i < INPUT_SIZE; i++) {
            w2[j][i] = w1[i][j] * (2.0f / INPUT_SIZE);
        }
    }
}

// Run Autoencoder forward propagation and return the MSE (Reconstruction Error)
static float run_autoencoder_inference(const float *x)
{
    float h[HIDDEN_SIZE] = {0};
    float y[INPUT_SIZE] = {0};
    
    // 1. Input -> Hidden layer
    for (int j = 0; j < HIDDEN_SIZE; j++) {
        float sum = 0.0f;
        for (int i = 0; i < INPUT_SIZE; i++) {
            sum += x[i] * w1[i][j];
        }
        // Activation function (tanh)
        h[j] = tanhf(sum);
    }
    
    // 2. Hidden -> Output layer (Reconstructed signal)
    for (int i = 0; i < INPUT_SIZE; i++) {
        float sum = 0.0f;
        for (int j = 0; j < HIDDEN_SIZE; j++) {
            sum += h[j] * w2[j][i];
        }
        y[i] = sum;
    }
    
    // 3. Compute Mean Squared Error (MSE)
    float mse = 0.0f;
    for (int i = 0; i < INPUT_SIZE; i++) {
        float diff = x[i] - y[i];
        mse += diff * diff;
    }
    mse = mse / INPUT_SIZE;
    
    return mse;
}

// Task simulating vibration sensor and running AI Autoencoder inference
static void ai_inference_task(void *arg)
{
    float signal[INPUT_SIZE];
    uint32_t ticks = 0;
    
    init_autoencoder_weights();
    ESP_LOGI(TAG, "Tiny AI Autoencoder initialized successfully!");

    while (1) {
        ticks++;
        
        // 1. Generate normal 50Hz sine wave sensor reading
        for (int i = 0; i < INPUT_SIZE; i++) {
            float t = (float)(ticks + i) / INPUT_SIZE;
            signal[i] = sinf(2.0f * M_PI * t);
            
            // Add tiny normal noise (variance 0.02)
            signal[i] += ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
            
            // 2. If anomaly is injected, add severe noise and spikes
            if (s_inject_anomaly) {
                // High frequency severe vibration anomalies
                signal[i] += sinf(8.0f * M_PI * t) * 0.8f;
                // Add random impulse spikes
                if (rand() % 3 == 0) {
                    signal[i] += ((float)rand() / RAND_MAX - 0.5f) * 1.5f;
                }
            }
        }
        
        // 3. Run AI Inference
        float mse = run_autoencoder_inference(signal);
        bool is_anomaly = (mse > 0.25f);
        
        // 4. Log state to terminal
        if (is_anomaly) {
            ESP_LOGW(TAG, "[AI Alert] Anomalous Vibration! MSE: %.4f", mse);
        } else {
            ESP_LOGI(TAG, "[AI] Normal Vibration. MSE: %.4f", mse);
        }
        
        // 5. Send report via MQTT to Web Dashboard
        if (s_mqtt_connected && s_mqtt_client != NULL) {
            char payload[128];
            snprintf(payload, sizeof(payload), "{\"mse\":%.4f,\"anomaly\":%s}", 
                     mse, is_anomaly ? "true" : "false");
                     
            esp_mqtt_client_publish(s_mqtt_client, MQTT_PUB_TOPIC, payload, 0, 1, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Run every 1 second
    }
}

// ==========================================
// MQTT Event Handler & Callbacks
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected successfully to broker!");
        s_mqtt_connected = true;
        msg_id = esp_mqtt_client_subscribe(client, MQTT_SUB_TOPIC, 0);
        ESP_LOGI(TAG, "Subscribed to topic: %s (msg_id=%d)", MQTT_SUB_TOPIC, msg_id);
        esp_mqtt_client_publish(client, MQTT_PUB_TOPIC, "{\"status\":\"online\"}", 0, 1, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected from broker");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT message received:");
        ESP_LOGI(TAG, "  Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "  Data: %.*s", event->data_len, event->data);

        // Process Commands
        if (strncmp(event->topic, MQTT_SUB_TOPIC, event->topic_len) == 0) {
            if (strncmp(event->data, "left", event->data_len) == 0) {
                both_motors_turn_left();
            } else if (strncmp(event->data, "right", event->data_len) == 0) {
                both_motors_turn_right();
            } else if (strncmp(event->data, "stop", event->data_len) == 0) {
                both_motors_stop();
            } else if (strncmp(event->data, "anomaly", event->data_len) == 0) {
                s_inject_anomaly = true;
                ESP_LOGW(TAG, "Command received: INJECTING VIBRATION ANOMALY");
            } else if (strncmp(event->data, "normal", event->data_len) == 0) {
                s_inject_anomaly = false;
                ESP_LOGI(TAG, "Command received: CLEARING ANOMALIES (Normal Mode)");
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT event error");
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to WiFi (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "WiFi disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start MQTT Client
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
        };
        s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_mqtt_client);
        ESP_LOGI(TAG, "MQTT Client initialized and started");
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization complete. Connecting to SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to AP SSID: %s", WIFI_SSID);
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting MQTT testing application with local Tiny AI...");
    wifi_init_sta();

    // Start AI inference task (runs 24/24 in background)
    xTaskCreate(ai_inference_task, "ai_inference_task", 4096, NULL, 5, NULL);
}
