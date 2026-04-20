// Three-button HAL — interrupt-driven so button events survive the e-paper
// full-refresh blocking window (~1.5 s per frame). Polling-only handling
// would miss any click that started and released during a refresh.
//
// We record the press timestamp in an ISR on falling edge, and the
// release timestamp on rising edge. buttons_poll() compares the two,
// decides click vs long press vs cancel, and emits at most one event per
// call. Long press is emitted on the ISR too (via the poll window checking
// `held >= LONG_MIN_MS` while still pressed).

#include "buttons.h"
#include "config.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEBOUNCE_MS  30
#define CLICK_MAX_MS 700       // was 500 — give the user a little slack
#define LONG_MIN_MS  1200

typedef struct {
    gpio_num_t gpio;
    volatile uint32_t press_ms;     // last press timestamp (0 = not pressed)
    volatile uint32_t release_ms;   // last release timestamp
    volatile bool     long_emitted; // once per press
    btn_event_t       click_evt;
    btn_event_t       long_evt;
} btn_t;

static btn_t s_btns[3];
static bool s_isr_service_installed = false;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void IRAM_ATTR btn_isr(void *arg) {
    btn_t *b = (btn_t *)arg;
    bool pressed = (gpio_get_level(b->gpio) == 0);
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (pressed) {
        b->press_ms = t;
        b->release_ms = 0;
        b->long_emitted = false;
    } else {
        b->release_ms = t;
    }
}

static void setup_btn(btn_t *b, gpio_num_t g,
                      btn_event_t click_evt, btn_event_t long_evt) {
    b->gpio = g;
    b->press_ms = 0;
    b->release_ms = 0;
    b->long_emitted = false;
    b->click_evt = click_evt;
    b->long_evt = long_evt;

    gpio_config_t c = {
        .pin_bit_mask = 1ULL << g,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&c);
    gpio_isr_handler_add(g, btn_isr, b);
}

void buttons_init(void) {
    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);
        s_isr_service_installed = true;
    }
    setup_btn(&s_btns[0], BTN_CONFIRM_GPIO, BTN_CONFIRM_CLICK, BTN_CONFIRM_LONG);
    setup_btn(&s_btns[1], BTN_UP_GPIO,      BTN_UP_CLICK,      BTN_UP_LONG);
    setup_btn(&s_btns[2], BTN_DOWN_GPIO,    BTN_DOWN_CLICK,    BTN_DOWN_LONG);
}

// Look at a single button's latched timestamps and decide whether to
// emit a click/long event. Returns true and writes `*out` if something
// fires. Called once per poll per button.
static bool check_one(btn_t *b, btn_event_t *out) {
    uint32_t t = now_ms();
    // Snapshot volatiles so the ISR can't change them mid-decision.
    uint32_t press   = b->press_ms;
    uint32_t release = b->release_ms;
    bool long_done   = b->long_emitted;

    // Long-press fires while still held past the threshold.
    if (press != 0 && release == 0 && !long_done) {
        if ((t - press) >= LONG_MIN_MS) {
            b->long_emitted = true;
            *out = b->long_evt;
            return true;
        }
    }

    // Click fires on release if it was a short hold.
    if (press != 0 && release != 0) {
        uint32_t held = release - press;
        // Clear the press window so we don't re-fire.
        b->press_ms = 0;
        b->release_ms = 0;
        if (!long_done && held >= DEBOUNCE_MS && held <= CLICK_MAX_MS) {
            *out = b->click_evt;
            return true;
        }
    }
    return false;
}

btn_event_t buttons_poll(void) {
    btn_event_t e = BTN_EVT_NONE;
    for (int i = 0; i < 3; i++) {
        if (check_one(&s_btns[i], &e)) return e;
    }
    return BTN_EVT_NONE;
}

bool buttons_confirm_held(void) {
    return s_btns[0].press_ms != 0 && s_btns[0].release_ms == 0;
}

uint32_t buttons_confirm_hold_ms(void) {
    uint32_t press = s_btns[0].press_ms;
    if (press == 0 || s_btns[0].release_ms != 0) return 0;
    return now_ms() - press;
}
