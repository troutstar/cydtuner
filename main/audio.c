#include "audio.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_common.h"
#include <math.h>
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_check.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio";

/*
 * Some older 2GB SD cards reject CMD59 (CRC_ON_OFF) with "illegal command".
 * CRC is optional in SPI mode. Wrap the internal sdmmc function to treat
 * NOT_SUPPORTED as success so the mount can proceed without CRC.
 */
esp_err_t __wrap_sdmmc_init_spi_crc(sdmmc_card_t *card)
{
    /* Card rejects CMD59 (CRC_ON_OFF). Never send it — sdspi_host sets
     * data_crc_enabled=1 the moment CMD59 is transmitted, even on rejection,
     * which then causes all subsequent data CRC checks to fail. Skip entirely;
     * data_crc_enabled stays 0 and transfers proceed without CRC. */
    (void)card;
    ESP_LOGW(TAG, "CMD59 skipped — card does not support SPI CRC");
    return ESP_OK;
}

#define AUDIO_BUF_SAMPLES 4096
#define WAV_PATH          "/sdcard/sweep.wav"

#define SD_PIN_CLK   18
#define SD_PIN_MOSI  23
#define SD_PIN_MISO  19
#define SD_PIN_CS     5

/* I2S pins — SPI peripheral header + Expand Pin header
 * BCK=IO18(SCK)  WS=IO23(MOSI)  DIN=IO35
 * MCLK not driven — module self-clocks from onboard 24.576MHz crystal */
#define I2S_PIN_BCK  18
#define I2S_PIN_WS   23
#define I2S_PIN_DIN  35

static i2s_chan_handle_t  s_i2s_rx   = NULL;

static audio_source_t s_source      = AUDIO_SOURCE_WAV_FILE;
static uint32_t       s_sample_rate = 0;
static uint16_t       s_channels    = 0;
static FILE          *s_wav_file    = NULL;
static uint32_t       s_data_start  = 0;
static uint32_t       s_data_end    = 0;

#ifdef PITCH_TEST_HARNESS
static volatile uint32_t s_position_bytes = 0;
static volatile float    s_synth_hz       = 440.0f;
static float             s_synth_phase    = 0.0f;
#endif

static int16_t s_stereo_buf[AUDIO_BUF_SAMPLES * 2];

static uint16_t read_u16(FILE *f) {
    uint8_t b[2]; fread(b, 1, 2, f);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t read_u32(FILE *f) {
    uint8_t b[4]; fread(b, 1, 4, f);
    return (uint32_t)(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24));
}

esp_err_t audio_init(audio_source_t source) {
    s_source = source;
#ifdef PITCH_TEST_HARNESS
    if (source == AUDIO_SOURCE_SYNTH) {
        s_sample_rate = 44100;
        s_synth_phase = 0.0f;
        ESP_LOGI(TAG, "synth source: %.1f Hz, %lu sample/s",
                 (double)s_synth_hz, (unsigned long)s_sample_rate);
        return ESP_OK;
    }
#endif
    if (source == AUDIO_SOURCE_I2S) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx), TAG, "I2S channel create failed");

        /* Left-justified format (MSB), left channel only.
         * Set module DIP switch to 1 (LJ) and jumper to Slave. */
        i2s_std_slot_config_t slot_cfg =
            I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
        slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
            .slot_cfg = slot_cfg,
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_PIN_BCK,
                .ws   = I2S_PIN_WS,
                .dout = I2S_GPIO_UNUSED,
                .din  = I2S_PIN_DIN,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S std init failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S enable failed");
        s_sample_rate = 44100;
        ESP_LOGI(TAG, "I2S source: WM8782S, %lu sample/s", (unsigned long)s_sample_rate);
        return ESP_OK;
    }
    if (source != AUDIO_SOURCE_WAV_FILE) return ESP_ERR_NOT_SUPPORTED;

    gpio_set_pull_mode(SD_PIN_MISO, GPIO_PULLUP_ONLY);

    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI, .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,  .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SD SPI init failed");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 4000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs  = SD_PIN_CS;
    slot.host_id  = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false, .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mnt, &card), TAG, "SD mount failed");
    ESP_LOGI(TAG, "SD card mounted");

    s_wav_file = fopen(WAV_PATH, "rb");
    if (!s_wav_file) { ESP_LOGE(TAG, "Cannot open %s", WAV_PATH); return ESP_ERR_NOT_FOUND; }

    char tag[5] = {0};
    fread(tag, 1, 4, s_wav_file); if (strncmp(tag, "RIFF", 4)) return ESP_ERR_INVALID_ARG;
    read_u32(s_wav_file);
    fread(tag, 1, 4, s_wav_file); if (strncmp(tag, "WAVE", 4)) return ESP_ERR_INVALID_ARG;

    bool found_fmt = false, found_data = false;
    while (!found_data) {
        if (fread(tag, 1, 4, s_wav_file) < 4) break;
        uint32_t sz = read_u32(s_wav_file);
        if (!strncmp(tag, "fmt ", 4)) {
            uint16_t fmt = read_u16(s_wav_file);
            if (fmt != 1) return ESP_ERR_INVALID_ARG;
            s_channels    = read_u16(s_wav_file);
            s_sample_rate = read_u32(s_wav_file);
            read_u32(s_wav_file); read_u16(s_wav_file);
            uint16_t bits = read_u16(s_wav_file);
            if (bits != 16) return ESP_ERR_INVALID_ARG;
            if (sz > 16) fseek(s_wav_file, sz - 16, SEEK_CUR);
            found_fmt = true;
        } else if (!strncmp(tag, "data", 4)) {
            s_data_start = (uint32_t)ftell(s_wav_file);
            s_data_end   = s_data_start + sz;
            found_data   = true;
        } else {
            fseek(s_wav_file, sz, SEEK_CUR);
        }
    }
    if (!found_fmt || !found_data) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "WAV: %lu Hz, %u ch", s_sample_rate, s_channels);
    return ESP_OK;
}

int audio_read(int16_t *buf, size_t len) {
    if (s_source == AUDIO_SOURCE_I2S) {
        if (!s_i2s_rx) return -1;
        /* Reuse s_stereo_buf as raw 32-bit staging area — same byte count */
        int32_t *raw = (int32_t *)s_stereo_buf;
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_i2s_rx, raw, len * sizeof(int32_t),
                                          &bytes_read, pdMS_TO_TICKS(200));
        if (err != ESP_OK) return -1;
        int got = (int)(bytes_read / sizeof(int32_t));
        /* INMP441: 24-bit audio left-justified in 32-bit slot → shift right 16 */
        for (int i = 0; i < got; i++)
            buf[i] = (int16_t)(raw[i] >> 16);
        return got;
    }
#ifdef PITCH_TEST_HARNESS
    if (s_source == AUDIO_SOURCE_SYNTH) {
        float hz     = s_synth_hz;
        float dphase = 2.0f * (float)M_PI * hz / (float)s_sample_rate;
        for (size_t i = 0; i < len; i++) {
            buf[i] = (int16_t)(sinf(s_synth_phase) * 16384.0f);
            s_synth_phase += dphase;
            if (s_synth_phase >= 2.0f * (float)M_PI)
                s_synth_phase -= 2.0f * (float)M_PI;
        }
        return (int)len;
    }
#endif
    if (!s_wav_file) return -1;
    if ((uint32_t)ftell(s_wav_file) >= s_data_end)
        fseek(s_wav_file, s_data_start, SEEK_SET);   /* loop */

    int got;
    if (s_channels == 1) {
        got = (int)fread(buf, sizeof(int16_t), len, s_wav_file);
    } else {
        size_t n = fread(s_stereo_buf, sizeof(int16_t), len * 2, s_wav_file) / 2;
        for (size_t i = 0; i < n; i++)
            buf[i] = (int16_t)(((int32_t)s_stereo_buf[i*2] + s_stereo_buf[i*2+1]) / 2);
        got = (int)n;
    }

#ifdef PITCH_TEST_HARNESS
    if (got > 0 && s_wav_file)
        s_position_bytes = (uint32_t)ftell(s_wav_file) - s_data_start;
#endif

    return got;
}

#ifdef PITCH_TEST_HARNESS
float audio_get_position_sec(void) {
    if (!s_wav_file || s_sample_rate == 0) return 0.0f;
    uint32_t bytes_per_sec = s_sample_rate * s_channels * 2;
    return (float)s_position_bytes / (float)bytes_per_sec;
}
#endif

uint32_t audio_get_sample_rate(void) { return s_sample_rate; }

#ifdef PITCH_TEST_HARNESS
void audio_synth_set_hz(float hz) { if (hz > 0.0f) s_synth_hz = hz; }
float audio_synth_get_hz(void) { return (s_source == AUDIO_SOURCE_SYNTH) ? s_synth_hz : 0.0f; }

void audio_set_source(audio_source_t src)
{
    if (src == s_source) return;
    s_source = src;
    if (src == AUDIO_SOURCE_SYNTH) {
        s_synth_phase = 0.0f;
        if (s_sample_rate == 0) s_sample_rate = 44100;
    } else if (src == AUDIO_SOURCE_WAV_FILE) {
        if (s_wav_file) {
            fseek(s_wav_file, s_data_start, SEEK_SET);
            s_position_bytes = 0;
        }
    }
}

audio_source_t audio_get_source(void) { return s_source; }
#endif
