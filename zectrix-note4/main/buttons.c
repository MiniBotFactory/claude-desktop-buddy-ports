// Three-button HAL for ZecTrix Note 4 — runs in its own 5 ms-tick task so
// button decisions are independent of the main loop's e-paper refresh
// stalls. The previous ISR-based version was unreliable on the Note 4
// front button: `gpio_get_level()` called from the ISR frequently read a
// bouncing mid-transition level, and the direction of the edge got
// reversed, leaving `press_ms` stuck and every short tap misread as a
// long press.
//
// Model:
//   * Poll the physical GPIO every 5 ms.
//   * Require 3 consecutive identical samples before switching state —
//     that's 15 ms of stable level, long enough to ride through typical
//     contact bounce.
//   * On a stable press, record press_ms (release_ms = 0, long_emitted = 0).
//   * On a stable release, record release_ms.
//   * Main thread calls buttons_poll() which atomically consumes the
//     latched press/release timestamps and decides click vs long.

#include "buttons.h"
#include "config.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define BTN_DEBUG 1
static const char *TAG = "btn";

#define SAMPLE_PERIOD_MS   5
#define STABLE_SAMPLES     3     // 5 ms * 3 = 15 ms debounce window
#define CLICK_MAX_MS    2500     // release within this = CLICK (Allow)
#define LONG_MIN_MS     2500     // held past this = LONG (Deny)

typedef struct {
    gpio_num_t gpio;
    // Debounce state (task-only):
    uint8_t  stable_level;     // last confirmed stable level (1 = released)
    uint8_t  streak_level;     // last sample
    uint8_t  streak;           // count of consecutive samples at streak_level

    // Latched events (shared with main):
    volatile uint32_t press_ms;
    volatile uint32_t release_ms;
    volatile uint32_t press_count;
    volatile uint32_t release_count;
    volatile bool     long_emitted;

    btn_event_t click_evt;
    btn_event_t long_evt;
    const char *name;
} btn_t;

static btn_t s_btns[3];

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void btn_task(void *arg) {
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        for (int i = 0; i < 3; i++) {
            btn_t *b = &s_btns[i];
            uint8_t now_lvl = gpio_get_level(b->gpio);
            if (now_lvl == b->streak_level) {
                if (b->streak < STABLE_SAMPLES) b->streak++;
            } else {
                b->streak_level = now_lvl;
                b->streak = 1;
            }

            // Settled into a new stable level?
            if (b->streak == STABLE_SAMPLES && now_lvl != b->stable_level) {
                b->stable_level = now_lvl;
                uint32_t t = now_ms();
                if (now_lvl == 0) {            // pressed
                    b->press_ms = t;
                    b->release_ms = 0;
                    b->long_emitted = false;
                    b->press_count++;
#if BTN_DEBUG
                    ESP_LOGI(TAG, "%s PRESS t=%u", b->name, (unsigned)t);
#endif
                } else {                       // released
                    if (b->press_ms != 0) {
                        b->release_ms = t;
                    }
                    b->release_count++;
#if BTN_DEBUG
                    ESP_LOGI(TAG, "%s RELEASE t=%u held=%ums",
                             b->name, (unsigned)t,
                             (unsigned)(b->press_ms ? t - b->press_ms : 0));
#endif
                }
            }
        }
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

static void setup_btn(int idx, gpio_num_t g,
                      btn_event_t click_evt, btn_event_t long_evt,
                      const char *name) {
    btn_t *b = &s_btns[idx];
    b->gpio = g;
    b->stable_level = 1;
    b->streak_level = 1;
    b->streak = STABLE_SAMPLES;
    b->press_ms = 0;
    b->release_ms = 0;
    b->press_count = 0;
    b->release_count = 0;
    b->long_emitted = false;
    b->click_evt = click_evt;
    b->long_evt = long_evt;
    b->name = name;

    gpio_config_t c = {
        .pin_bit_mask = 1ULL << g,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
#if BTN_DEBUG
    ESP_LOGI(TAG, "wired %s on GPIO %d (init level=%d)", name, g, gpio_get_level(g));
#endif
}

void buttons_init(void) {
    setup_btn(0, BTN_CONFIRM_GPIO, BTN_CONFIRM_CLICK, BTN_CONFIRM_LONG, "confirm");
    setup_btn(1, BTN_UP_GPIO,      BTN_UP_CLICK,      BTN_UP_LONG,      "up");
    setup_btn(2, BTN_DOWN_GPIO,    BTN_DOWN_CLICK,    BTN_DOWN_LONG,    "down");

    xTaskCreatePinnedToCore(btn_task, "btn", 2048, NULL, 5, NULL, 0);
}

static bool check_one(btn_t *b, btn_event_t *out) {
    uint32_t t = now_ms();
    uint32_t press   = b->press_ms;
    uint32_t release = b->release_ms;
    bool long_done   = b->long_emitted;

    // LONG fires while still held past threshold.
    if (press != 0 && release == 0 && !long_done) {
        if ((t - press) >= LONG_MIN_MS) {
            b->long_emitted = true;
            *out = b->long_evt;
            return true;
        }
    }

    // CLICK fires on release if held within window and no LONG fired.
    if (press != 0 && release != 0) {
        uint32_t held = release - press;
        b->press_ms = 0;
        b->release_ms = 0;
        if (!long_done && held <= CLICK_MAX_MS) {
            *out = b->click_evt;
            return true;
        }
    }
    return false;
}

btn_event_t buttons_poll(void) {
#if BTN_DEBUG
    static uint32_t last_log_ms = 0;
    uint32_t now = now_ms();
    if (now - last_log_ms > 5000) {
        last_log_ms = now;
        ESP_LOGI(TAG,
                 "state confirm:P%u/R%u stable=%d | up:P%u/R%u stable=%d | down:P%u/R%u stable=%d",
                 (unsigned)s_btns[0].press_count, (unsigned)s_btns[0].release_count, s_btns[0].stable_level,
                 (unsigned)s_btns[1].press_count, (unsigned)s_btns[1].release_count, s_btns[1].stable_level,
                 (unsigned)s_btns[2].press_count, (unsigned)s_btns[2].release_count, s_btns[2].stable_level);
    }
#endif
    btn_event_t e = BTN_EVT_NONE;
    for (int i = 0; i < 3; i++) {
        if (check_one(&s_btns[i], &e)) {
#if BTN_DEBUG
            ESP_LOGI(TAG, "%s event=%d", s_btns[i].name, (int)e);
#endif
            return e;
        }
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
