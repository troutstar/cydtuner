#include "ili9341.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9341";

#define LCD_PIN_CS    15
#define LCD_PIN_DC     2
#define LCD_PIN_CLK   14
#define LCD_PIN_MOSI  13
#define LCD_PIN_MISO  12
#define LCD_PIN_BL    21
#define LCD_WIDTH    320
#define LCD_HEIGHT   240

static spi_device_handle_t s_spi = NULL;

static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(LCD_PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *data, size_t len) {
    if (!len) return;
    gpio_set_level(LCD_PIN_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_byte(uint8_t b) { lcd_data(&b, 1); }

esp_err_t ili9341_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LCD_PIN_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI, .miso_io_num = LCD_PIN_MISO,
        .sclk_io_num = LCD_PIN_CLK,  .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "bus init failed");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0, .spics_io_num = LCD_PIN_CS, .queue_size = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "dev add failed");

    lcd_cmd(0xCF); lcd_data((uint8_t[]){0x00,0x83,0x30}, 3);
    lcd_cmd(0xED); lcd_data((uint8_t[]){0x64,0x03,0x12,0x81}, 4);
    lcd_cmd(0xE8); lcd_data((uint8_t[]){0x85,0x01,0x79}, 3);
    lcd_cmd(0xCB); lcd_data((uint8_t[]){0x39,0x2C,0x00,0x34,0x02}, 5);
    lcd_cmd(0xF7); lcd_byte(0x20);
    lcd_cmd(0xEA); lcd_data((uint8_t[]){0x00,0x00}, 2);
    lcd_cmd(0xC0); lcd_byte(0x26);
    lcd_cmd(0xC1); lcd_byte(0x11);
    lcd_cmd(0xC5); lcd_data((uint8_t[]){0x35,0x3E}, 2);
    lcd_cmd(0xC7); lcd_byte(0xBE);
    lcd_cmd(0x36); lcd_byte(0xE8);   /* MADCTL: landscape 180°, BGR */
    lcd_cmd(0x3A); lcd_byte(0x55);   /* RGB565 */
    lcd_cmd(0xB1); lcd_data((uint8_t[]){0x00,0x1B}, 2);
    lcd_cmd(0xF2); lcd_byte(0x08);
    lcd_cmd(0x26); lcd_byte(0x01);
    lcd_cmd(0xE0); lcd_data((uint8_t[]){0x1F,0x1A,0x18,0x0A,0x0F,0x06,0x45,0x87,0x32,0x0A,0x07,0x02,0x07,0x05,0x00}, 15);
    lcd_cmd(0xE1); lcd_data((uint8_t[]){0x00,0x25,0x27,0x05,0x10,0x09,0x3A,0x78,0x4D,0x05,0x18,0x0D,0x38,0x3A,0x1F}, 15);
    lcd_cmd(0xB7); lcd_byte(0x07);
    lcd_cmd(0xB6); lcd_data((uint8_t[]){0x0A,0x82,0x27,0x00}, 4);
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x29);

    gpio_set_level(LCD_PIN_BL, 1);
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (!w || !h) return ESP_OK;
    lcd_cmd(0x2A); lcd_data((uint8_t[]){x>>8, x&0xFF, (x+w-1)>>8, (x+w-1)&0xFF}, 4);
    lcd_cmd(0x2B); lcd_data((uint8_t[]){y>>8, y&0xFF, (y+h-1)>>8, (y+h-1)&0xFF}, 4);
    lcd_cmd(0x2C);
    gpio_set_level(LCD_PIN_DC, 1);
    static uint8_t row[LCD_WIDTH * 2];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < w; i++) { row[i*2] = hi; row[i*2+1] = lo; }
    for (int r = 0; r < h; r++) {
        spi_transaction_t t = { .length = w*16, .tx_buffer = row };
        spi_device_polling_transmit(s_spi, &t);
    }
    return ESP_OK;
}

esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    return ili9341_fill_rect(x, y, 1, 1, color);
}

esp_err_t ili9341_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    if (!w || !h) return ESP_OK;
    lcd_cmd(0x2A); lcd_data((uint8_t[]){x>>8, x&0xFF, (x+w-1)>>8, (x+w-1)&0xFF}, 4);
    lcd_cmd(0x2B); lcd_data((uint8_t[]){y>>8, y&0xFF, (y+h-1)>>8, (y+h-1)&0xFF}, 4);
    lcd_cmd(0x2C);
    gpio_set_level(LCD_PIN_DC, 1);
    spi_transaction_t t = { .length = (size_t)w * h * 16, .tx_buffer = pixels };
    return spi_device_polling_transmit(s_spi, &t);
}
