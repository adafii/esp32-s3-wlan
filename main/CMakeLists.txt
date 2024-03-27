idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")

nvs_create_partition_image(nvs ../nvs_data.csv FLASH_IN_PROJECT)
