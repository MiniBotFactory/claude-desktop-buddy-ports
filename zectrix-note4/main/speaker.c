// ES8311 + NS4168 speaker path for ZecTrix Note 4.
//
// Uses the esp_codec_dev component: we supply I2S data channel, I2C
// control channel, and a GPIO for the NS4168 PA shutdown line; it
// handles ES8311 register init, volume, sample-rate plumbing. All we
// do on top is generate short square-wave tones for notification beeps.

#include "speaker.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

static const char *TAG = "spk";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2s_chan_handle_t       s_tx_chan = NULL;
static esp_codec_dev_handle_t  s_dev = NULL;
static bool                    s_ready = false;

#define SAMPLE_RATE 16000

static esp_err_t init_power(void) {
    // Bring up the audio rail — ES8311 and NS4168 are both downstream of
    // AUDIO_PWR_PIN (GPIO 42). Reference config says force high.
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << AUDIO_PWR_PIN) | (1ULL << AUDIO_PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&c);
    gpio_set_level(AUDIO_PWR_PIN, 1);
    gpio_set_level(AUDIO_PA_PIN, 0);       // PA off until we have audio
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t init_i2c(void) {
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t init_i2s(void) {
    i2s_chan_config_t ch_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&ch_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws   = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din  = AUDIO_I2S_DIN,
            .invert_flags = { 0 },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_tx_chan);
}

static esp_err_t init_codec(void) {
    // Data: our existing I2S TX channel (codec_dev will write to it).
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    // Control: I2C bus at 0x18.
    audio_codec_i2c_cfg_t ctl_cfg = {
        .port = I2C_NUM_0,
        .addr = AUDIO_ES8311_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctl_cfg);
    if (!ctrl_if) return ESP_FAIL;

    // GPIO interface — esp_codec_dev uses it for the PA pin.
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = AUDIO_PA_PIN,
        .use_mclk = true,
        .hw_gain = { 0 },
        .pa_reverted = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) return ESP_FAIL;

    // Open output at our sample rate.
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .mclk_multiple = 256,
    };
    esp_err_t err = esp_codec_dev_open(s_dev, &fs);
    if (err != ESP_OK) return err;

    esp_codec_dev_set_out_vol(s_dev, 70);   // 0-100, comfortable default
    return ESP_OK;
}

esp_err_t speaker_init(void) {
    esp_err_t err;
    err = init_power(); if (err != ESP_OK) { ESP_LOGW(TAG, "power fail 0x%x", err); return err; }
    err = init_i2c();   if (err != ESP_OK) { ESP_LOGW(TAG, "i2c fail 0x%x", err);   return err; }
    err = init_i2s();   if (err != ESP_OK) { ESP_LOGW(TAG, "i2s fail 0x%x", err);   return err; }
    err = init_codec(); if (err != ESP_OK) { ESP_LOGW(TAG, "codec fail 0x%x", err); return err; }
    s_ready = true;
    ESP_LOGI(TAG, "ready (ES8311, 16 kHz mono)");
    return ESP_OK;
}

// Fill a buffer with `n` samples of a square wave at `freq_hz`. Amplitude
// 20% to avoid amp clipping — can raise later if too quiet.
static void fill_square(int16_t *buf, size_t n, uint16_t freq_hz) {
    if (freq_hz == 0) { memset(buf, 0, n * sizeof(int16_t)); return; }
    int period_samples = SAMPLE_RATE / freq_hz;
    if (period_samples < 2) period_samples = 2;
    int half = period_samples / 2;
    const int16_t lo = -6500, hi = 6500;
    for (size_t i = 0; i < n; i++) {
        buf[i] = ((i % period_samples) < half) ? hi : lo;
    }
}

void speaker_tone(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_ready) return;

    size_t nsamples = (size_t)SAMPLE_RATE * duration_ms / 1000;
    if (nsamples == 0) return;

    // Work in 256-sample chunks so we don't need a giant buffer.
    static int16_t chunk[256];
    fill_square(chunk, 256, freq_hz);

    size_t written = 0;
    while (written < nsamples) {
        size_t n = nsamples - written;
        if (n > 256) n = 256;
        esp_codec_dev_write(s_dev, chunk, n * sizeof(int16_t));
        written += n;
    }
}

void speaker_ding(void) {
    speaker_tone(880, 80);      // A5
    speaker_tone(1320, 150);    // E6
}

void speaker_deny(void) {
    speaker_tone(300, 140);
    speaker_tone(200, 200);
}

void speaker_allow(void) {
    speaker_tone(660, 80);      // E5
    speaker_tone(880, 80);      // A5
    speaker_tone(1320, 120);    // E6
}
