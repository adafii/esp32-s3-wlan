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
#define PER_CHANNEL_SCAN_TIME_MS 1000
#define MIN_CHANNEL 1
#define MAX_CHANNEL 11

#define SCAN_EVENT "scan_event"
#define CHANNEL_CHANGE_EVENT 1
#define NEW_STATION_EVENT 2

#define BUFFER_SIZE 600

typedef uint8_t packet_t[BUFFER_SIZE];

typedef struct {
    struct libwifi_bss data[MAX_STATIONS_NUM];
    uint32_t size;
} station_records_t;

typedef struct {
    int8_t len;
    char const* title;
} column_t;

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

void channel_change_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static uint8_t current_channel = MIN_CHANNEL;
    current_channel = (current_channel - MIN_CHANNEL + 1) % (MAX_CHANNEL - MIN_CHANNEL + 1) + MIN_CHANNEL;
    ESP_ERROR_CHECK(esp_wifi_set_channel(current_channel, 0));
}

void new_station_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static const column_t columns[] = {{5, "rssi"}, {6 * 3 - 1 + 2, "bssid"}, {32, "ssid"}, {9, "channel"},
                                       {5, "wps"},  {15, "encryption"}};
    static bool has_header = false;

    if (!has_header) {
        for (int i = 0; i < sizeof(columns) / sizeof(column_t); ++i) {
            printf("%*s", columns[i].len, columns[i].title);
        }
        printf("\n");
        has_header = true;
    }

    struct libwifi_bss* station = event_data;
    char security_buffer[LIBWIFI_SECURITY_BUF_LEN];
    libwifi_get_security_type(station, security_buffer);

    printf("%*d", columns[0].len, station->signal);
    printf("  %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", station->bssid[0], station->bssid[1], station->bssid[2],
           station->bssid[3], station->bssid[4], station->bssid[5]);
    printf("%*.*s", columns[2].len, columns[2].len, station->hidden ? "<hidden>" : station->ssid);
    printf("%*d", columns[3].len, station->channel);
    printf("%*s", columns[4].len, station->wps ? "Yes" : "No");
    printf("%*.*s", columns[5].len, columns[5].len, security_buffer);
    printf("\n");
}

bool gptimer_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* user_ctx) {
    BaseType_t task_unblocked = pdFALSE;
    esp_event_isr_post_to(scan_loop_handle, SCAN_EVENT, CHANNEL_CHANGE_EVENT, NULL, 0, &task_unblocked);
    return task_unblocked;
};

esp_err_t start_channel_change_timer() {
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = PER_CHANNEL_SCAN_TIME_MS * 1000, .reload_count = 0, .flags.auto_reload_on_alarm = true};
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t timer_cb = {.on_alarm = gptimer_alarm_cb};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &timer_cb, NULL));

    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    return ESP_OK;
}

void wifi_promiscuous_cb(void* buffer, wifi_promiscuous_pkt_type_t type) {
    size_t buffer_size = ((wifi_pkt_rx_ctrl_t*)(buffer))->sig_len + sizeof(wifi_pkt_rx_ctrl_t);

    if (buffer_size > sizeof(packet_t)) {
        ESP_LOGW(TAG, "Buffer size was larger than packet_t size (%d > %d) -> skipped", buffer_size, sizeof(packet_t));
        return;
    }

    if (xQueueSendToBack(packet_queue, (packet_t*)buffer, 100) != pdPASS) {
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

void save_station_task(void* user) {
    packet_t received_packet = {};

    for (;;) {
        if (xQueueReceive(packet_queue, &received_packet, 100) != pdPASS) {
            continue;
        }

        wifi_pkt_rx_ctrl_t* header = (wifi_pkt_rx_ctrl_t*)received_packet;
        size_t data_size = header->sig_len;
        uint8_t* data = received_packet + sizeof(wifi_pkt_rx_ctrl_t);

        struct libwifi_frame frame = {0};
        if (libwifi_get_wifi_frame(&frame, data, data_size, false)) {
            ESP_LOGE(TAG, "Could not parse wifi frame");
            continue;
        }

        if (frame.frame_control.subtype != SUBTYPE_BEACON) {
            libwifi_free_wifi_frame(&frame);
            continue;
        }

        struct libwifi_bss bss = {0};
        if (libwifi_parse_beacon(&bss, &frame)) {
            ESP_LOGE(TAG, "Could not parse beacon");
            libwifi_free_wifi_frame(&frame);
            continue;
        }

        bool is_already_recorded = false;
        for (uint32_t station = 0; station < stations.size; ++station) {
            if (is_same_bssid(bss.bssid, stations.data[station].bssid)) {
                is_already_recorded = true;
                stations.data[station].signal = header->rssi;
                break;
            }
        }

        if (!is_already_recorded && stations.size <= MAX_STATIONS_NUM) {
            bss.signal = header->rssi;
            memcpy(&stations.data[stations.size], &bss, sizeof(struct libwifi_bss));
            esp_event_post_to(scan_loop_handle, SCAN_EVENT, NEW_STATION_EVENT, &stations.data[stations.size],
                              sizeof(struct libwifi_bss), 100);
            ++stations.size;
        }

        libwifi_free_bss(&bss);
        libwifi_free_wifi_frame(&frame);
    }
}

esp_err_t init_promiscuous_mode() {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    wifi_promiscuous_filter_t prom_filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&prom_filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    return ESP_OK;
}

esp_err_t scan_beacons() {
    ESP_ERROR_CHECK(init_event_loops());
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(scan_loop_handle, SCAN_EVENT, CHANNEL_CHANGE_EVENT,
                                                             channel_change_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(scan_loop_handle, SCAN_EVENT, NEW_STATION_EVENT,
                                                             new_station_handler, NULL, NULL));

    ESP_ERROR_CHECK(init_wifi());

    packet_queue = xQueueCreate(30, sizeof(packet_t));
    vQueueAddToRegistry(packet_queue, "Packet queue");

    ESP_ERROR_CHECK(init_promiscuous_mode());
    ESP_ERROR_CHECK(start_channel_change_timer());

    if (xTaskCreatePinnedToCore(save_station_task, "Saves station data", 10 * 1024, NULL, 10, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}