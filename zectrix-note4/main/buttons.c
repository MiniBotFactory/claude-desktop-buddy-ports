// Three-button HAL — simple polling + debouncing, no interrupts.
//
// Design model (matches the AtomS3R port's UX):
//   * confirm short click → Allow (fast path)
//   * confirm long-press  → Deny (2 second hold, with live progress ui)
//   * up short click      → cycle dashboard pages (not wired in v0.1)
//   * down short click    → same
//   * down long-press     → power off intent (up-stream decides)

#include "buttons.h"
#include "config.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEBOUNCE_MS  30
#define CLICK_MAX_MS 500
#define LONG_MIN_MS  1200

typedef struct {
    gpio_num_t gpio;
    bool       pressed;
    uint32_t   press_start_ms;
    bool       long_emitted;
} btn_t;

static btn_t s_confirm, s_up, s_down;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline bool read_pressed(gpio_num_t g) {
    // Buttons are active-low, pull-up. Pressed = level 0.
    return gpio_get_level(g) == 0;
}

static void setup_pin(gpio_num_t g) {
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << g,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
}

void buttons_init(void) {
    setup_pin(BTN_CONFIRM_GPIO);
    setup_pin(BTN_UP_GPIO);
    setup_pin(BTN_DOWN_GPIO);
    s_confirm = (btn_t){ BTN_CONFIRM_GPIO, false, 0, false };
    s_up      = (btn_t){ BTN_UP_GPIO,      false, 0, false };
    s_down    = (btn_t){ BTN_DOWN_GPIO,    false, 0, false };
}

// Internal: update one button state + emit event via *out when something
// just happened. Returns true if an event was produced.
static bool tick(btn_t *b, btn_event_t click_evt, btn_event_t long_evt,
                 btn_event_t *out) {
    uint32_t t = now_ms();
    bool pressed = read_pressed(b->gpio);

    if (pressed && !b->pressed) {
        // Rising edge → record start, debounce
        b->press_start_ms = t;
        b->pressed = true;
        b->long_emitted = false;
        return false;
    }

    if (pressed && b->pressed) {
        // Still held — check long threshold
        if (!b->long_emitted && (t - b->press_start_ms) >= LONG_MIN_MS) {
            b->long_emitted = true;
            *out = long_evt;
            return true;
        }
        return false;
    }

    if (!pressed && b->pressed) {
        // Released — decide click or nothing
        b->pressed = false;
        uint32_t held = t - b->press_start_ms;
        if (held < DEBOUNCE_MS) return false;
        if (!b->long_emitted && held <= CLICK_MAX_MS) {
            *out = click_evt;
            return true;
        }
    }
    return false;
}

btn_event_t buttons_poll(void) {
    btn_event_t e = BTN_EVT_NONE;
    if (tick(&s_confirm, BTN_CONFIRM_CLICK, BTN_CONFIRM_LONG, &e)) return e;
    if (tick(&s_up,      BTN_UP_CLICK,      BTN_UP_LONG,      &e)) return e;
    if (tick(&s_down,    BTN_DOWN_CLICK,    BTN_DOWN_LONG,    &e)) return e;
    return BTN_EVT_NONE;
}

bool buttons_confirm_held(void) { return s_confirm.pressed; }

uint32_t buttons_confirm_hold_ms(void) {
    if (!s_confirm.pressed) return 0;
    uint32_t t = now_ms();
    return t - s_confirm.press_start_ms;
}
