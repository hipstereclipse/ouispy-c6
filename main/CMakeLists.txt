idf_component_register(
    SRCS
        "main.c"
        "app_common.c"
        "display.c"
        "map_tile.c"
        "led_ctrl.c"
        "buzzer.c"
        "button.c"
        "nvs_store.c"
        "wifi_manager.c"
        "ble_scanner.c"
        "web_server.c"
        "sniffer.c"
        "flock_you.c"
        "fox_hunter.c"
        "sky_spy.c"
            "storage_ext.c"
    INCLUDE_DIRS "." "${IDF_PATH}/components/bt/host/nimble/esp-hci/include"
    REQUIRES
        bt
        esp_app_format
        esp_http_server
        esp_https_server
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_lcd
        esp_lcd_jd9853
        nvs_flash
        json
            fatfs
            sdmmc
            esp_driver_sdspi
    EMBED_TXTFILES "index.html" "certs/servercert.pem" "certs/prvtkey.pem"
)

if(NOT DEFINED BOARD_TOUCH)
    set(BOARD_TOUCH 0)
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_TOUCH=${BOARD_TOUCH})
