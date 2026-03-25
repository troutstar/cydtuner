idf_component_register(
    SRCS "main.c" "wifi.c" "httpd.c" "test_harness.c"
    # "../../main" headers needed once pitch.c / audio.c are added to SRCS in Plan 2.
    # Inert in Plan 1 but listed now so CMakeLists.txt does not need editing later.
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
)

# audio.c defines __wrap_sdmmc_init_spi_crc to suppress CMD59 on older SD cards.
# The __wrap_ symbol is only present when audio.c is in SRCS (Plan 2).
# Enabling the linker wrap without that symbol causes an "undefined reference" link
# error if anything in the binary calls sdmmc_init_spi_crc. Uncomment in Plan 2.
# target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_init_spi_crc")

# Activates pitch_detect_full() and audio_get_position_sec() in pitch.c / audio.c.
# Inert in Plan 1 (those files are not in SRCS yet). Uncomment in Plan 2 alongside
# the linker wrap when audio.c and pitch.c are added to SRCS.
# target_compile_definitions(${COMPONENT_LIB} PRIVATE PITCH_TEST_HARNESS=1)
