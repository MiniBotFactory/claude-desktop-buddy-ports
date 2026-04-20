// Minimal 1bpp framebuffer painter for the 400x300 SSD2683.
//
// The framebuffer is kept in regular heap RAM (PSRAM is available on
// Note 4 but the frame is only 15 KB, fits in internal too). Pixel
// convention: bit value 1 = white, 0 = black. MSB is the left pixel.
//
// What's here:
//   - framebuffer alloc + clear
//   - horizontal / vertical lines, filled rects, rounded-ish rects
//   - text at four sizes (5x7 built-in font scaled 1x, 2x, 3x, 4x)
//   - basic progress bar
// That's enough for the dashboard + prompt screens. If you need anti-
// aliasing, richer fonts (CJK), or images, hook LVGL in later.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);                        // alloc fb, fill white
uint8_t *ui_framebuf(void);                // for epd_full_refresh
void ui_clear(bool white);

void ui_pixel(int x, int y, bool white);
void ui_hline(int x, int y, int w, bool white);
void ui_vline(int x, int y, int h, bool white);
void ui_rect(int x, int y, int w, int h, bool white);      // outline
void ui_fill(int x, int y, int w, int h, bool white);      // filled
void ui_frame(int x, int y, int w, int h, int t);          // t-pixel black frame

// Text rendering. scale = 1..4, monochrome black on white fb.
// Returns the x position after the last drawn glyph (for chaining).
int  ui_text(int x, int y, const char *s, int scale);
int  ui_text_center(int cx, int y, const char *s, int scale);
int  ui_text_width(const char *s, int scale);

// Simple horizontal progress bar. val 0..max drawn black inside a 2-pixel
// black border. Works for a token bar etc.
void ui_progress(int x, int y, int w, int h, int val, int max);

#ifdef __cplusplus
}
#endif
