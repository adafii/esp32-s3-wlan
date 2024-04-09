#include "driver/gptimer.h"
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
#define CHANNEL_SCAN_TIME_MS 3000
#define MIN_CHANNEL 1
#define MAX_CHANNEL 11

#define SCAN_EVENT "scan_event"
#define CHANNEL_CHANGE_EVENT 1

typedef struct {
    wifi_promiscuous_pkt_type_t type;
    size_t data_size;
    uint8_t data[512];
} packet_t;

typedef struct {
    struct libwifi_bss data[MAX_STATIONS_NUM];
    uint32_t size;
} station_records_t;

static const char* TAG = "scan_beacons";
static QueueHandle_t packet_queue;
static station_records_t stations = {};
static esp_event_loop_handle_t scan_loop_handle;

esp_err_t init_wifi() {
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

esp_err_t init_event_loops() {
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_event_loop_args_t event_loop_args = {.queue_size = 10,
                                             .task_name = "Scan event loop",
                                             .task_priority = 10,
                                             .task_stack_size = 10 * 1024,
                                             .task_core_id = 1};

    ESP_ERROR_CHECK(esp_event_loop_create(&event_loop_args, &scan_loop_handle));

    return ESP_OK;
}

bool gptimer_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* user_ctx) {
    BaseType_t task_unblocked = pdFALSE;
    esp_event_isr_post_to(scan_loop_handle, SCAN_EVENT, CHANNEL_CHANGE_EVENT, NULL, 0, &task_unblocked);
    return task_unblocked;
};

void channel_change_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static uint8_t current_channel = MIN_CHANNEL;
    current_channel = (current_channel - MIN_CHANNEL + 1) % (MAX_CHANNEL - MIN_CHANNEL + 1) + MIN_CHANNEL;
    ESP_ERROR_CHECK(esp_wifi_set_channel(current_channel, 0));
}

esp_err_t start_channel_change_timer() {
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = CHANNEL_SCAN_TIME_MS * 1000, .reload_count = 0, .flags.auto_reload_on_alarm = true};
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t timer_cb = {.on_alarm = gptimer_alarm_cb};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &timer_cb, NULL));

    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    return ESP_OK;
}

void wifi_promiscuous_cb(void* buffer, wifi_promiscuous_pkt_type_t type) {
    packet_t received_packet = {};

    received_packet.type = type;
    received_packet.data_size = ((wifi_pkt_rx_ctrl_t*)(buffer))->sig_len;

    if (received_packet.data_size == 0 || received_packet.data_size > 512) {
        ESP_LOGW(TAG, "Packet data size was %d -> skipped", received_packet.data_size);
        return;
    }

    memcpy(received_packet.data, (uint8_t*)(buffer) + sizeof(wifi_pkt_rx_ctrl_t), received_packet.data_size);

    if (xQueueSendToBack(packet_queue, &received_packet, 0) != pdPASS) {
        ESP_LOGW(TAG, "Packet queue is full, packet dropped");
    }
}

bool is_same_bssid(const uint8_t* bssid1, const uint8_t* bssid2) {
    if (!bssid1 || !bssid2) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        if (bssid1[i] != bssid2[i]) {
            return false;
        }
    }

    return true;
}

void save_station_data(void* user) {
    packet_t received_packet = {};
    for (;;) {
        if (xQueueReceive(packet_queue, &received_packet, 10) != pdPASS) {
            continue;
        }

        uint8_t* data = received_packet.data;

        if (received_packet.type != WIFI_PKT_MGMT) {
            continue;
        }

        struct libwifi_frame frame = {0};
        if (libwifi_get_wifi_frame(&frame, data, received_packet.data_size, false)) {
            ESP_LOGE(TAG, "Could not parse wifi frame");
            continue;
        }

        if (frame.frame_control.subtype != SUBTYPE_BEACON) {
            continue;
        }

        struct libwifi_bss bss = {0};
        if (libwifi_parse_beacon(&bss, &frame)) {
            ESP_LOGE(TAG, "Could not parse beacon");
            continue;
        }

        bool is_already_recorded = false;
        for (uint32_t sta = 0; sta < stations.size; ++sta) {
            if (is_same_bssid(bss.bssid, stations.data[sta].bssid)) {
                is_already_recorded = true;
                break;
            }
        }

        if (is_already_recorded) {
            continue;
        }

        if (stations.size == MAX_STATIONS_NUM) {
            ESP_LOGW(TAG, "Couldn't add new station: maximum number of stations recorded");
            continue;
        }

        memcpy(&stations.data[stations.size], &bss, sizeof(struct libwifi_bss));
        ++stations.size;

        for (uint32_t sta = 0; sta < stations.size; ++sta) {
            printf("%lu %s ", sta, stations.data[sta].ssid);
        }
        printf("\n");
    }
}

esp_err_t scan_beacons() {
    ESP_ERROR_CHECK(init_wifi());

    packet_queue = xQueueCreate(30, sizeof(packet_t));
    vQueueAddToRegistry(packet_queue, "Packet queue");

    init_event_loops();
    esp_event_handler_instance_register_with(scan_loop_handle, SCAN_EVENT, CHANNEL_CHANGE_EVENT, channel_change_handler,
                                             NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    wifi_promiscuous_filter_t prom_filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&prom_filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(start_channel_change_timer());

    if (xTaskCreatePinnedToCore(save_station_data, "Saves station data", 10 * 1024, NULL, 10, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}