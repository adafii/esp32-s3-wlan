#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdio.h>

static const char* TAG = "wlan";

// Private API definitions
typedef struct {
    const char* ssid;
    const char* wpa_psk;
} wlan_config_t;

esp_err_t init();
esp_err_t scan(wifi_ap_record_t** ap_records, uint16_t* ap_num);
esp_err_t connect();

char const* get_auth_type(wifi_auth_mode_t auth);
void print_ap_record(const wifi_ap_record_t ap_records[], const uint16_t* ap_num);
esp_err_t get_wlan_config(wlan_config_t* wlan_config);

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

esp_err_t get_wlan_config(wlan_config_t* wlan_config) {
    nvs_handle_t nvs_handle = {};
    ESP_ERROR_CHECK(nvs_open("wlan_config", NVS_READONLY, &nvs_handle));

    wlan_config->ssid = read_nvs_str(&nvs_handle, "ssid");
    wlan_config->wpa_psk = read_nvs_str(&nvs_handle, "wpa_psk");

    return ESP_OK;
}

esp_err_t connect() {
    return ESP_OK;
}

void test() {
    init();
    wifi_ap_record_t* ap_records = NULL;
    uint16_t ap_num = 0;

    scan(&ap_records, &ap_num);
    print_ap_record(ap_records, &ap_num);
    free(ap_records);

    /*    wlan_config_t wlan_config = {};
        get_wlan_config(&wlan_config);
        printf("SSID: %s\n", wlan_config.ssid);
        printf("WPA PSK: %s\n", wlan_config.wpa_psk);*/
}