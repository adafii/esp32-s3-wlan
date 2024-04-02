#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "wlan";

// Private API definitions
typedef struct {
    const char* ssid;
    const char* wpa_psk;
} nvs_wifi_config_t;

esp_err_t init();
esp_err_t scan(wifi_ap_record_t** ap_records, uint16_t* ap_num);
esp_err_t connect(const nvs_wifi_config_t* config);

char const* get_auth_type(wifi_auth_mode_t auth);
void print_ap_record(const wifi_ap_record_t ap_records[], const uint16_t* ap_num);
esp_err_t get_wlan_config(nvs_wifi_config_t* wlan_config);

// Auth mode type to string
char const* get_auth_type(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_ENTERPRISE:
            return "EAP";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        default:
            return "?";
    }
}

// Init nvs and wifi
esp_err_t init() {
    esp_err_t nvs_error = nvs_flash_init();

    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_error = nvs_flash_init();
    }

    ESP_ERROR_CHECK(nvs_error);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
};

void print_ap_record(const wifi_ap_record_t* ap_records, const uint16_t* ap_num) {
    if (!ap_records || *ap_num == 0) {
        return;
    }

    typedef struct {
        int8_t len;
        char const* title;
    } column;

    column const columns[] = {{19, "bssid"}, {20, "ssid"}, {10, "channel"}, {10, "rssi"}, {10, "auth"}};

    for (int i = 0; i < sizeof(columns) / sizeof(column); ++i) {
        printf("%*s", columns[i].len, columns[i].title);
    }

    puts("");

    for (int ap = 0; ap < *ap_num; ++ap) {
        printf("  %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", ap_records[ap].bssid[0], ap_records[ap].bssid[1],
               ap_records[ap].bssid[2], ap_records[ap].bssid[3], ap_records[ap].bssid[4], ap_records[ap].bssid[5]);
        printf("%*.*s", columns[1].len, columns[1].len, ap_records[ap].ssid);
        printf("%*d", columns[2].len, ap_records[ap].primary);
        printf("%*d", columns[3].len, ap_records[ap].rssi);
        printf("%*.*s\n", columns[4].len, columns[4].len, get_auth_type(ap_records[ap].authmode));
    }

    puts("");
}

// Scan local wlans
esp_err_t scan(wifi_ap_record_t** ap_records, uint16_t* ap_num) {
    static wifi_scan_config_t scan_config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 1000, .max = 1500}},
    };

    ESP_LOGI(TAG, "Scanning...\n");
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(ap_num));
    ESP_LOGI(TAG, "APs found: %d\n\n", *ap_num);

    *ap_records = calloc(*ap_num, sizeof(**ap_records));

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(ap_num, *ap_records));
    return ESP_OK;
}

char* read_nvs_str(const nvs_handle_t* handle, const char* key) {
    size_t value_size;
    ESP_ERROR_CHECK(nvs_get_str(*handle, key, NULL, &value_size));
    char* str = malloc(value_size);
    ESP_ERROR_CHECK(nvs_get_str(*handle, key, str, &value_size));

    return str;
}

esp_err_t get_wlan_config(nvs_wifi_config_t* wlan_config) {
    nvs_handle_t nvs_handle = {};
    ESP_ERROR_CHECK(nvs_open("wlan_config", NVS_READONLY, &nvs_handle));

    wlan_config->ssid = read_nvs_str(&nvs_handle, "ssid");
    wlan_config->wpa_psk = read_nvs_str(&nvs_handle, "wpa_psk");

    return ESP_OK;
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 4) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t connect(const nvs_wifi_config_t* config) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_t instance_any_id = {};
    esp_event_handler_instance_t instance_got_ip = {};

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK, };
    strncpy((char*)(&wifi_config.sta.ssid), config->ssid, 32);
    strncpy((char*)(&wifi_config.sta.password), config->wpa_psk, 32);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to ap SSID: %s", config->ssid);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", config->ssid);
    } else {
        ESP_LOGE(TAG, "Unexpected event");
    }

    return ESP_OK;
}

void test() {
    init();
/*    wifi_ap_record_t* ap_records = NULL;
    uint16_t ap_num = 0;

    scan(&ap_records, &ap_num);
    print_ap_record(ap_records, &ap_num);
    free(ap_records);*/

    nvs_wifi_config_t nvs_wifi_config = {};
    get_wlan_config(&nvs_wifi_config);
    ESP_ERROR_CHECK(connect(&nvs_wifi_config));
}