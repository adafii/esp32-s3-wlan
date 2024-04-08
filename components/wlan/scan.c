#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libwifi.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#define MAX_STATIONS_NUM 30
#define SNIFF_CHANNEL 5

static const char* TAG = "scan_beacons";
static QueueHandle_t packet_queue;

typedef struct {
    wifi_promiscuous_pkt_type_t type;
    wifi_pkt_rx_ctrl_t* header;
    uint8_t* data;
} packet_t;

esp_err_t init() {
    esp_err_t nvs_error = nvs_flash_init();

    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_error = nvs_flash_init();
    }

    ESP_ERROR_CHECK(nvs_error);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
};

void wifi_promiscuous_cb(void* buffer, wifi_promiscuous_pkt_type_t type) {
    packet_t received_packet = {};

    received_packet.type = type;
    received_packet.header = malloc(sizeof(wifi_pkt_rx_ctrl_t));
    memcpy(received_packet.header, buffer, sizeof(wifi_pkt_rx_ctrl_t));

    uint32_t data_offset = sizeof(wifi_pkt_rx_ctrl_t);
    size_t data_len = received_packet.header->sig_len;
    if (data_len > 0) {
        received_packet.data = malloc(data_len);
        memcpy(received_packet.data, buffer + data_offset, data_len);
    }

    if (xQueueSendToBack(packet_queue, &received_packet, 0) != pdPASS) {
        ESP_LOGW(TAG, "Queue full, packet dropped");
    }
}

void print_packets_task(void* user) {
    packet_t received_packet = {};
    for (;;) {
        if (xQueueReceive(packet_queue, &received_packet, 10) != pdPASS) {
            continue;
        }

        if (received_packet.type != WIFI_PKT_MGMT) {
            continue;
        }

        wifi_pkt_rx_ctrl_t* header = received_packet.header;
        uint8_t* data = received_packet.data;
        size_t data_len = received_packet.header->sig_len;

        struct libwifi_frame frame = {0};
        if (libwifi_get_wifi_frame(&frame, data, data_len, false)) {
            ESP_LOGE(TAG, "Could not parse wifi frame");
            goto packet_cleanup;
        }

        if (frame.frame_control.subtype != SUBTYPE_BEACON) {
            goto packet_cleanup;
        }

        struct libwifi_bss bss = {0};
        if (libwifi_parse_beacon(&bss, &frame)) {
            ESP_LOGE(TAG, "Could not parse beacon");
            goto packet_cleanup;
        }

        printf("ESSID: %s\n", bss.hidden ? "(hidden)" : bss.ssid);
        printf("BSSID: " MACSTR "\n", MAC2STR(bss.bssid));
        printf("Channel: %d\n", bss.channel);
        printf("WPS: %s\n", bss.wps ? "Yes" : "No");

    packet_cleanup:
        free(header);
        free(data);
    }
}

esp_err_t scan_beacons() {
    ESP_ERROR_CHECK(init());

    packet_queue = xQueueCreate(30, sizeof(packet_t));
    vQueueAddToRegistry(packet_queue, "Packet queue");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SNIFF_CHANNEL, 0));

    wifi_promiscuous_filter_t prom_filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&prom_filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));

    BaseType_t task_err = xTaskCreatePinnedToCore(print_packets_task, "Prints packets", 10 * 1024, NULL, 10, NULL, 1);

    if (task_err != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}