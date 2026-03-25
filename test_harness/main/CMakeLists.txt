idf_component_register(
    SRCS "main.c" "wifi.c" "httpd.c" "test_harness.c"
         "../../main/pitch.c" "../../main/audio.c"
    # "../../main" headers needed for pitch.h, audio.h, etc.
    INCLUDE_DIRS "." "../../main"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        mdns
        nvs_flash
        esp_http_server
        json
        esp_timer
        driver
        sdmmc
        fatfs
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_init_spi_crc")

target_compile_definitions(${COMPONENT_LIB} PRIVATE PITCH_TEST_HARNESS=1)
