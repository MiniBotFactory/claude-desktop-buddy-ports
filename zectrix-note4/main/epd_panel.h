// Minimal SSD2683 e-paper driver for ZecTrix Note 4 (400x300, 1bpp).
//
// Extracted from ZecTrix's custom_lcd_display.cc with LVGL and all the
// throttling / dirty-rect logic stripped out. What's left is just:
//   - SPI + GPIO init
//   - EPD power on / reset / command / data / busy-wait
//   - Full-frame 1bpp refresh (SSD2683 uses a packed 2bpp wire format, so
//     we translate 1bpp→2bpp on the fly via pack_1bpp_to_2683)
//
// Only full refresh is implemented in v0.1. Partial refresh can be added
// later — see comments in EPD_DisplayPart in the reference driver.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Framebuffer is 1bpp, 0xFF = white, 0x00 = black, row-major, MSB first.
// Size = EPD_WIDTH/8 * EPD_HEIGHT = 50 * 300 = 15000 bytes.
#define EPD_1BPP_STRIDE    (400 / 8)
#define EPD_FRAMEBUF_BYTES (EPD_1BPP_STRIDE * 300)

// One-time bring-up: power rail + SPI bus + GPIOs + SSD2683 OTP init.
esp_err_t epd_init(void);

// Clear the SSD2683 on-chip RAM to white (writes 0xFF for the whole frame).
void epd_clear_white(void);

// Push `fb` (must be EPD_FRAMEBUF_BYTES long) to the panel and trigger a
// full-screen refresh. Blocks until the BUSY line releases (~1.5 s).
void epd_full_refresh(const uint8_t *fb);

// Put the panel into deep sleep (call before app sleep to save the ~50 µA
// panel standby current).
void epd_power_off(void);

#ifdef __cplusplus
}
#endif
