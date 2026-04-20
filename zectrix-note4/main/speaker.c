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
    // Power sequence matches the ZecTrix reference firmware:
    //   1. VBAT_PWR_PIN (GPIO 17) high     — main 3V3 rail for EVERYTHING
    //   2. AUDIO_PWR_PIN (GPIO 42) high   — downstream gate for ES8311+NS4168
    //   3. AUDIO_PA_PIN  (GPIO 46) low    — amp stays off until we emit
    //
    // We previously only drove AUDIO_PWR high without VBAT, so the ES8311
    // stayed cold and every I2C write timed out. Use gpio_hold_en so the
    // level survives light/deep sleep transitions.
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << VBAT_PWR_PIN)
                      | (1ULL << AUDIO_PWR_PIN)
                      | (1ULL << AUDIO_PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&c);

    gpio_hold_dis(VBAT_PWR_PIN);
    gpio_set_level(VBAT_PWR_PIN, 1);
    gpio_hold_en(VBAT_PWR_PIN);

    gpio_hold_dis(AUDIO_PWR_PIN);
    gpio_set_level(AUDIO_PWR_PIN, 1);
    gpio_hold_en(AUDIO_PWR_PIN);

    gpio_set_level(AUDIO_PA_PIN, 0);

    // ES8311 needs ~25 ms after power to respond to I2C. Give it a bit more
    // to be safe — we're doing this once at boot, the delay is irrelevant.
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

// Drive the NS4168 amplifier's shutdown pin active/inactive. The PA pin
// passed to esp_codec_dev isn't actually toggled by the codec driver —
// the ZecTrix reference firmware manages it manually, so we do too.
static void pa_enable(bool on) {
    gpio_hold_dis(AUDIO_PA_PIN);
    gpio_set_level(AUDIO_PA_PIN, on ? 1 : 0);
    gpio_hold_en(AUDIO_PA_PIN);
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

// Probe the I2C bus by trying to talk to the ES8311 address. This is a
// blind write of zero bytes — ACK means the device is alive.
static esp_err_t probe_codec(void) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AUDIO_ES8311_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_probe(s_i2c_bus, AUDIO_ES8311_ADDR, 50);
    i2c_master_bus_rm_device(dev);
    return err;
}

// Scan the full 7-bit I2C address range; log every address that ACKs.
// Invaluable when the codec "isn't there" — we find out what IS there.
static void scan_i2c_bus(void) {
    ESP_LOGI(TAG, "I2C scan on SDA=%d SCL=%d:", AUDIO_I2C_SDA, AUDIO_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 10) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X ACK", addr);
            found++;
        }
    }
    if (!found) ESP_LOGW(TAG, "  no devices ACK'd — power or wiring");
}

esp_err_t speaker_init(void) {
    esp_err_t err;
    err = init_power(); if (err != ESP_OK) { ESP_LOGW(TAG, "power fail 0x%x", err); return err; }
    err = init_i2c();   if (err != ESP_OK) { ESP_LOGW(TAG, "i2c fail 0x%x", err);   return err; }

    // IMPORTANT: ES8311 ignores I2C until MCLK is actually running. We
    // must bring the I2S channel up first so MCLK = 256 * 16 kHz streams
    // into the codec's pin 2. If we probe/open the codec before MCLK,
    // every I2C write times out with "Fail to write to dev 18".
    err = init_i2s();   if (err != ESP_OK) { ESP_LOGW(TAG, "i2s fail 0x%x", err);   return err; }

    // Give the clocks + AVDD rail a moment to settle before talking.
    vTaskDelay(pdMS_TO_TICKS(50));

    // Diagnostic: scan so we know WHICH devices are on the bus.
    scan_i2c_bus();

    err = probe_codec();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 probe at 0x%02X failed (0x%x). "
                      "Likely power-on sequence or wrong pins.",
                      AUDIO_ES8311_ADDR, err);
        return err;
    }
    ESP_LOGI(TAG, "ES8311 ACK'd at 0x%02X", AUDIO_ES8311_ADDR);

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

    pa_enable(true);                        // unmute the NS4168 before audio

    static int16_t chunk[256];
    fill_square(chunk, 256, freq_hz);

    size_t written = 0;
    while (written < nsamples) {
        size_t n = nsamples - written;
        if (n > 256) n = 256;
        esp_codec_dev_write(s_dev, chunk, n * sizeof(int16_t));
        written += n;
    }

    // Keep PA on between back-to-back tones; individual ding/deny/allow
    // functions call speaker_tone in sequence so constantly toggling would
    // click. We turn PA off on quiet (handled by caller via speaker_mute).
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
