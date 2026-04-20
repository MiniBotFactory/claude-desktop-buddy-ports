// SSD2683 e-paper driver, port of the relevant parts of
// ZecTrix's main/boards/zectrix-s3-epaper-4.2/custom_lcd_display.cc.
//
// Differences from the upstream driver:
//   - No LVGL, no task, no dirty-rect / debounce logic. Callers hand us a
//     framebuffer and ask for a full refresh; that's it.
//   - C instead of C++.
//   - Temperature compensation values and init-sequence bytes are kept
//     identical to upstream so the panel's optical behaviour is the same.

#include "epd_panel.h"
#include "config.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "epd";

static spi_device_handle_t s_spi = NULL;
static bool s_bus_inited = false;

// Previous frame, kept only for partial refresh. Allocated in PSRAM on
// first partial call. If NULL, epd_partial_refresh falls back to full.
static uint8_t *s_prev_fb = NULL;

// ---------- tiny GPIO helpers -----------------------------------------------
static inline void set_cs(int level)  { gpio_set_level(EPD_CS_PIN,  level); }
static inline void set_dc(int level)  { gpio_set_level(EPD_DC_PIN,  level); }
static inline void set_rst(int level) { gpio_set_level(EPD_RST_PIN, level); }

static void read_busy(void) {
    // BUSY is active-low: the panel pulls it low while busy and releases
    // it to high when done. Poll every 5ms, no timeout (matches upstream).
    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------- SPI wrappers ----------------------------------------------------
static void spi_send_byte(uint8_t b) {
    spi_transaction_t t = { .length = 8, .tx_buffer = &b };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static uint8_t spi_recv_byte(void) {
    uint8_t rx = 0;
    spi_transaction_t t = { .length = 8, .rx_buffer = &rx };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    return rx;
}

static void spi_write_bytes(const uint8_t *buf, size_t len) {
    spi_transaction_t t = { .length = 8 * len, .tx_buffer = buf };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

// ---------- EPD primitives --------------------------------------------------
static void epd_send_cmd(uint8_t cmd) {
    set_dc(0);
    set_cs(0);
    spi_send_byte(cmd);
    set_cs(1);
}

static void epd_send_data(uint8_t data) {
    set_dc(1);
    set_cs(0);
    spi_send_byte(data);
    set_cs(1);
}

static uint8_t epd_recv_data(void) {
    set_dc(1);
    set_cs(0);
    uint8_t d = spi_recv_byte();
    set_cs(1);
    return d;
}

static void epd_write_bytes(const uint8_t *buf, size_t len) {
    set_dc(1);
    set_cs(0);
    spi_write_bytes(buf, len);
    set_cs(1);
}

// ---------- Power + reset ---------------------------------------------------
static void epd_power_on(void) {
    gpio_hold_dis(EPD_PWR_PIN);
    gpio_set_level(EPD_PWR_PIN, 1);
    gpio_hold_en(EPD_PWR_PIN);
}

void epd_power_off(void) {
    gpio_hold_dis(EPD_PWR_PIN);
    gpio_set_level(EPD_PWR_PIN, 0);
    gpio_hold_en(EPD_PWR_PIN);
}

// Pack 8 bits of 1bpp source into two SSD2683-format output bytes. Each
// source bit becomes a 2-bit cell in the output — bit 0x01 = white (11),
// bit 0x00 = black (00). The wire format interleaves two adjacent source
// bits into the 8-bit output. Copied verbatim from the reference driver.
static inline void pack_1bpp(uint8_t in, uint8_t *out0, uint8_t *out1) {
    uint8_t b0 = 0, b1 = 0;
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t bit = (in >> (7 - i)) & 0x01;
        if (i < 4) b0 |= bit << (8 - 2 * (i + 1));
        else       b1 |= bit << (14 - 2 * i);
    }
    *out0 = b0;
    *out1 = b1;
}

// ---------- Public API ------------------------------------------------------
// Panel wake-up: power on + reset pulse + OTP init. Must run before every
// refresh because epd_turn_on_display() drops the power rail at the end.
static void epd_wake_and_init(void) {
    epd_power_on();
    vTaskDelay(pdMS_TO_TICKS(10));
    set_rst(1); vTaskDelay(pdMS_TO_TICKS(10));
    set_rst(0); vTaskDelay(pdMS_TO_TICKS(20));
    set_rst(1); vTaskDelay(pdMS_TO_TICKS(10));
    read_busy();

    // SSD2683 OTP init sequence — copied from ZecTrix driver.
    epd_send_cmd(0x00);
    epd_send_data(0x2F);
    epd_send_data(0x2E);
    epd_send_cmd(0xE9);
    epd_send_data(0x01);
    read_busy();
}

esp_err_t epd_init(void) {
    // GPIOs
    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << EPD_DC_PIN) | (1ULL << EPD_CS_PIN)
                      | (1ULL << EPD_RST_PIN) | (1ULL << EPD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gc);

    gpio_config_t gi = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gi);

    set_cs(1);
    set_dc(1);
    set_rst(1);

    // SPI bus + device (once; bus stays up across refreshes)
    if (!s_bus_inited) {
        spi_bus_config_t bus = {
            .mosi_io_num = EPD_MOSI_PIN,
            .miso_io_num = -1,
            .sclk_io_num = EPD_SCK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = EPD_FRAMEBUF_BYTES * 2 + 64,
        };
        esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) return err;

        spi_device_interface_config_t dev = {
            .clock_speed_hz = 8 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = -1,     // CS is driven manually
            .queue_size = 7,
        };
        err = spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi);
        if (err != ESP_OK) return err;
        s_bus_inited = true;
    }

    // Power up the panel for the first time so the boot splash can draw.
    // Every subsequent epd_full_refresh() re-wakes the panel internally.
    epd_wake_and_init();

    ESP_LOGI(TAG, "EPD initialized (%dx%d)", EPD_WIDTH, EPD_HEIGHT);
    return ESP_OK;
}

void epd_clear_white(void) {
    // We don't send a clear command — instead send an all-0xFF framebuffer
    // which epd_full_refresh handles correctly.
    static uint8_t empty[EPD_FRAMEBUF_BYTES];
    memset(empty, 0xFF, sizeof(empty));
    epd_full_refresh(empty);
}

// Turn-on sequence — trigger the actual display update and power off.
static void epd_turn_on_display(void) {
    epd_send_cmd(0x04);              // power on
    read_busy();
    epd_send_cmd(0x12);              // display refresh
    epd_send_data(0x00);
    read_busy();
    epd_send_cmd(0x02);              // power off
    epd_send_data(0x00);
    read_busy();
    epd_power_off();
}

// Ensure prev_buffer exists and contains a copy of `fb`. Used at the end
// of every refresh so the next partial refresh has something to diff
// against. PSRAM-backed — 15 KB internal would fit too but PSRAM is
// plentiful and framebuffers are naturally PSRAM-friendly.
static void update_prev_buffer(const uint8_t *fb) {
    if (!s_prev_fb) {
        s_prev_fb = heap_caps_malloc(EPD_FRAMEBUF_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_prev_fb) {
            s_prev_fb = heap_caps_malloc(EPD_FRAMEBUF_BYTES, MALLOC_CAP_8BIT);
        }
    }
    if (s_prev_fb) memcpy(s_prev_fb, fb, EPD_FRAMEBUF_BYTES);
}

void epd_full_refresh(const uint8_t *fb) {
    if (!fb) return;

    // Re-wake the panel. epd_turn_on_display() at the end of the previous
    // refresh dropped the rail, so commands won't land until we power back
    // on and re-init. This mirrors the upstream driver's EPD_Init()+Display()
    // cycle.
    epd_wake_and_init();

    // Temperature compensation — sample internal sensor, pick an 0xE6 value
    // based on the result. Matches the upstream table.
    epd_send_cmd(0x40);
    read_busy();
    uint8_t temp1 = epd_recv_data();
    uint8_t tempvalue;
    if      (temp1 <=   5) tempvalue = 232;  // -24
    else if (temp1 <=  10) tempvalue = 235;  // -21
    else if (temp1 <=  20) tempvalue = 238;  // -18
    else if (temp1 <=  30) tempvalue = 241;  // -15
    else if (temp1 <= 127) tempvalue = 244;  // -12
    else                    tempvalue = 232;

    epd_send_cmd(0xE0); epd_send_data(0x02);
    epd_send_cmd(0xE6); epd_send_data(tempvalue);
    epd_send_cmd(0xA5);
    read_busy();
    vTaskDelay(pdMS_TO_TICKS(10));

    // Transmit the framebuffer. SSD2683 wants 2bpp packed — each row is
    // 400 pixels * 2 bits = 100 bytes. We pack on the fly row by row.
    epd_send_cmd(0x10);
    const int bytes_in  = EPD_1BPP_STRIDE;          // 50
    const int bytes_out = EPD_1BPP_STRIDE * 2;      // 100
    uint8_t line[EPD_1BPP_STRIDE * 2];
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *src = fb + y * bytes_in;
        uint8_t *dst = line;
        for (int xb = 0; xb < bytes_in; xb++) {
            uint8_t a, b;
            pack_1bpp(src[xb], &a, &b);
            *dst++ = a;
            *dst++ = b;
        }
        epd_write_bytes(line, bytes_out);
    }

    epd_turn_on_display();
    update_prev_buffer(fb);
}

// Partial refresh: pack prev + new frame bits together (2 bits per source
// pixel: high = prev, low = new). SSD2683 computes the diff internally
// and only updates pixels that actually changed, skipping the flash cycle.
void epd_partial_refresh(const uint8_t *fb) {
    if (!fb) return;
    if (!s_prev_fb) {
        // First refresh ever — no baseline to diff, do a full.
        epd_full_refresh(fb);
        return;
    }

    epd_wake_and_init();

    // Temperature compensation (same table as full). SSD2683 appears to
    // accept partial updates without the 0xE0/0xE6/0xA5 block, but the
    // reference driver re-sends it so ghosting stays consistent — do the
    // same.
    epd_send_cmd(0x40);
    read_busy();
    uint8_t temp1 = epd_recv_data();
    uint8_t tempvalue;
    if      (temp1 <=   5) tempvalue = 232;
    else if (temp1 <=  10) tempvalue = 235;
    else if (temp1 <=  20) tempvalue = 238;
    else if (temp1 <=  30) tempvalue = 241;
    else if (temp1 <= 127) tempvalue = 244;
    else                    tempvalue = 232;
    epd_send_cmd(0xE0); epd_send_data(0x02);
    epd_send_cmd(0xE6); epd_send_data(tempvalue);
    epd_send_cmd(0xA5);
    read_busy();
    vTaskDelay(pdMS_TO_TICKS(10));

    epd_send_cmd(0x10);
    read_busy();

    // Each row: interleave bits so that bit 2k   = new[bit k] and
    //                                   bit 2k+1 = prev[bit k].
    // This matches the reference EPD_DisplayPart exactly.
    const int bytes_per_row = EPD_1BPP_STRIDE;      // 50
    uint8_t line[EPD_1BPP_STRIDE * 2];
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *prev_row = s_prev_fb + y * bytes_per_row;
        const uint8_t *new_row  = fb        + y * bytes_per_row;

        for (int j = 0; j < bytes_per_row; j++) {
            uint8_t pb = prev_row[j];
            uint8_t nb = new_row[j];
            uint16_t result = 0;
            for (int k = 0; k < 8; k++) {
                int src_bit  = 7 - k;
                int dst_bit0 = 2 * src_bit;       // even bit = new frame
                int dst_bit1 = 2 * src_bit + 1;   // odd  bit = prev frame
                result |= ((uint16_t)((pb >> src_bit) & 1u)) << dst_bit1;
                result |= ((uint16_t)((nb >> src_bit) & 1u)) << dst_bit0;
            }
            line[2 * j + 0] = (uint8_t)(result >> 8);
            line[2 * j + 1] = (uint8_t)(result & 0xFF);
        }
        epd_write_bytes(line, EPD_1BPP_STRIDE * 2);
    }

    epd_turn_on_display();
    update_prev_buffer(fb);
}
