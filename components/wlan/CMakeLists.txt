idf_component_register(
    SRCS "wlan.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "esp_wifi nvs_flash"
)
