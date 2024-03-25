#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdio.h>

// Init NVS and wifi
esp_err_t init() {
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
};

typedef struct {
    int8_t len;
    char const* title;
} column;

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

// Test wifi by scanning local wlans
esp_err_t scan() {
    wifi_scan_config_t scan_config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 1000, .max = 1500}},
    };

    puts("------------------------------------------------------------------------");

    printf("Scanning...\n");
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    printf("APs found: %d\n\n", ap_num);

    wifi_ap_record_t* ap_records = calloc(ap_num, sizeof(wifi_ap_record_t));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_records));

    column const columns[] = {{19, "bssid"}, {20, "ssid"}, {10, "channel"}, {10, "rssi"}, {10, "auth"}};

    for (int i = 0; i < sizeof(columns) / sizeof(column); ++i) {
        printf("%*s", columns[i].len, columns[i].title);
    }

    puts("");

    for (int i = 0; i < ap_num; ++i) {
        printf("  %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", ap_records[i].bssid[0], ap_records[i].bssid[1],
               ap_records[i].bssid[2], ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);
        printf("%*.*s", columns[1].len, columns[1].len, ap_records[i].ssid);
        printf("%*d", columns[2].len, ap_records[i].primary);
        printf("%*d", columns[3].len, ap_records[i].rssi);
        printf("%*.*s", columns[4].len, columns[4].len, get_auth_type(ap_records[i].authmode));
        puts("");
    }

    puts("");

    free(ap_records);

    return ESP_OK;
}

esp_err_t connect() {

    return ESP_OK;
}

void test() {
    init();
    scan();
}