// Three-button HAL for ZecTrix Note 4.
//   BTN_CONFIRM (front face)  = GPIO 0
//   BTN_UP   (side up)        = GPIO 39
//   BTN_DOWN (side down)      = GPIO 18   — shared with VBAT_PWR
//
// Each button emits two events: short click (press+release within 500 ms)
// and long press (held for 1200 ms). A long-held DOWN is treated as the
// user's intent to power off and must be handled up-stream.

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BTN_EVT_NONE       = 0,
    BTN_CONFIRM_CLICK  = 1,
    BTN_CONFIRM_LONG   = 2,
    BTN_UP_CLICK       = 3,
    BTN_UP_LONG        = 4,
    BTN_DOWN_CLICK     = 5,
    BTN_DOWN_LONG      = 6,
} btn_event_t;

#ifdef __cplusplus
extern "C" {
#endif

void buttons_init(void);
btn_event_t buttons_poll(void);     // call each tick; non-blocking
bool buttons_confirm_held(void);    // for live progress display during long-press
uint32_t buttons_confirm_hold_ms(void);

#ifdef __cplusplus
}
#endif
