idf_component_register(
    SRCS "main.c" "audio.c" "pitch.c" "display.c" "touch.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES ili9341 xpt2046 fatfs sdmmc esp_timer driver esp_wifi nvs_flash lwip
)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sdmmc_init_spi_crc" APPEND)
