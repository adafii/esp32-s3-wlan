idf_component_register(
    SRCS wlan.c scan.c
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "esp_wifi nvs_flash libwifi"
)
