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
// SSD2683 always uses the full black-to-white-to-target sequence here
// ("flash refresh"), so the screen appears to invert briefly — the
// trade-off is zero ghosting. Prefer epd_partial_refresh for frequent
// state-change updates (dashboard counter ticks, etc.).
void epd_full_refresh(const uint8_t *fb);

// Push `fb` and trigger a partial-area refresh based on the diff against
// the last frame sent. Only pixels that actually changed flip, so there
// is no black-white flash. After many partial refreshes residual ghosting
// can build up — callers should periodically force a full refresh
// (roughly every 10-20 partial updates) to clear it. Blocks ~600-900 ms.
//
// Falls back to a full refresh automatically the very first call (no
// previous frame exists) or if internal allocation fails.
void epd_partial_refresh(const uint8_t *fb);

// Put the panel into deep sleep (call before app sleep to save the ~50 µA
// panel standby current).
void epd_power_off(void);

#ifdef __cplusplus
}
#endif
