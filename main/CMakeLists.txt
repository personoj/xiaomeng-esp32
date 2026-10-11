idf_component_register(SRCS "main.c" "board.c" "display.c" "avatar_renderer.c" "playback.c" "speech.c" "ptt_capture.c" "transport.c" "vendor/esp_lcd_st77916.c"
    INCLUDE_DIRS "." "vendor"
    EMBED_FILES "prompts/wake.pcm" "prompts/sleep.pcm"
    REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_ledc esp_driver_spi esp_lcd esp_wifi esp_event esp_netif nvs_flash esp_partition esp_timer mbedtls)
