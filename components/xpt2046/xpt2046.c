#include "xpt2046.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "xpt2046";

#define TOUCH_PIN_CS    33
#define TOUCH_PIN_CLK   25
#define TOUCH_PIN_MOSI  32
#define TOUCH_PIN_MISO  39
#define TOUCH_PIN_IRQ   36

static spi_device_handle_t s_spi = NULL;

static uint16_t xpt_send(uint8_t cmd) {
    uint8_t tx[3] = {cmd, 0, 0}, rx[3] = {0};
    spi_transaction_t t = { .length = 24, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
    return ((rx[1] << 8) | rx[2]) >> 3;
}

esp_err_t xpt2046_init(void) {
    gpio_config_t irq = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq);

    spi_bus_config_t bus = {
        .mosi_io_num = TOUCH_PIN_MOSI, .miso_io_num = TOUCH_PIN_MISO,
        .sclk_io_num = TOUCH_PIN_CLK,  .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED), TAG, "bus init failed");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0, .spics_io_num = TOUCH_PIN_CS, .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI3_HOST, &dev, &s_spi), TAG, "dev add failed");

    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

bool xpt2046_read(int *x, int *y, int *pressure) {
    if (gpio_get_level(TOUCH_PIN_IRQ) != 0) return false;
    *x        = xpt_send(0xD0);
    *y        = xpt_send(0x90);
    *pressure = xpt_send(0xB0);
    return true;
}
