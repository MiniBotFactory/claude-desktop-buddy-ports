// Claude Desktop Buddy — ZecTrix Note 4 (v0.1)
// ============================================================================
// E-paper variant of the Hardware Buddy. Shows Claude Desktop session state
// on the 4.2" monochrome panel and takes Allow/Deny decisions via the three
// hardware buttons. Because e-paper refresh is slow, the UI is designed as
// a static dashboard that only redraws on meaningful state change.
//
// UI states
//   BOOT      → splash "Claude Buddy" + "advertising"
//   PAIRING   → 6-digit passkey centred big
//   DASHBOARD → counters, tokens progress, last msg
//   PROMPT    → "PERMISSION" band, tool + hint, "Confirm=Allow  Hold=Deny"
//
// Input
//   confirm short click              → Allow
//   confirm long press (≥1.2 s)      → Deny (with visible countdown)
//   up / down short click            → cycle dashboard pages (future)
//   down long press                  → power-off intent (future)
//
// See README.md for the open TODO list — especially audio + deep sleep.

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "epd_panel.h"
#include "ble_nus.h"
#include "ui_paint.h"
#include "buttons.h"
#include "proto.h"

static const char *TAG = "buddy";

static proto_snapshot_t g_snap;
static char g_last_prompt_id[48];
static char g_last_replied_id[48];

// Line-assembly for inbound BLE RX.
static char   g_line[1536];
static size_t g_line_len = 0;

static void pump_ble(void) {
    uint8_t chunk[128];
    while (true) {
        size_t n = ble_nus_read(chunk, sizeof(chunk));
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (g_line_len > 0) {
                    g_line[g_line_len] = 0;
                    proto_apply(&g_snap, g_line);
                    g_line_len = 0;
                }
            } else if (g_line_len < sizeof(g_line) - 1) {
                g_line[g_line_len++] = c;
            }
        }
    }
}

static void send_permission(const char *id, const char *decision) {
    char buf[128];
    int n = proto_fmt_permission(buf, sizeof(buf), id, decision);
    if (n > 0) ble_nus_write((const uint8_t*)buf, n);
    ESP_LOGI(TAG, "[tx] permission %s (%s)", decision, id);
    strncpy(g_last_replied_id, id, sizeof(g_last_replied_id) - 1);
    g_last_replied_id[sizeof(g_last_replied_id) - 1] = 0;
}

// ---- Rendering --------------------------------------------------------------

static void render_pairing(uint32_t pk) {
    ui_clear(true);                                      // white background
    ui_text_center(200, 20, "CLAUDE BUDDY", 2);
    ui_fill(40, 60, 320, 2, false);                       // divider bar

    ui_text_center(200, 100, "Pairing", 3);
    char buf[8];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)pk);
    ui_text_center(200, 150, buf, 6);                     // big 6-digit passkey
    ui_text_center(200, 240, "Type this on the desktop dialog", 2);
    epd_full_refresh(ui_framebuf());
}

static const char *persona_label(persona_t p) {
    switch (p) {
        case PERSONA_SLEEP:     return "OFFLINE";
        case PERSONA_IDLE:      return "READY";
        case PERSONA_BUSY:      return "WORKING";
        case PERSONA_ATTENTION: return "ATTENTION";
    }
    return "?";
}

static void render_dashboard(void) {
    ui_clear(true);

    // Header band
    ui_fill(0, 0, 400, 32, false);
    // Invert: draw title in white on black band (cheap: use ui_text but
    // on a black bar, we'd need white glyphs — for v0.1 skip the invert
    // and just paint the title beside the bar)
    ui_fill(10, 6, 220, 20, true);                       // white cut-out
    ui_text(18, 10, "CLAUDE BUDDY", 2);

    // BLE status on the right of the header
    const char *sig = !ble_nus_connected() ? "advert"
                    : !ble_nus_secure()    ? "paired"
                                            : "secure";
    ui_fill(300, 6, 90, 20, true);
    ui_text(308, 10, sig, 2);

    // State big label
    persona_t p = proto_persona(&g_snap);
    ui_text_center(200, 58, persona_label(p), 4);

    // Counters
    char line[32];
    snprintf(line, sizeof(line), "running  %u", g_snap.running);
    ui_text(40, 120, line, 2);
    snprintf(line, sizeof(line), "waiting  %u", g_snap.waiting);
    ui_text(40, 145, line, 2);
    snprintf(line, sizeof(line), "total    %u", g_snap.total);
    ui_text(40, 170, line, 2);

    // Tokens progress (capped at 1M)
    snprintf(line, sizeof(line), "%lu tokens today", (unsigned long)g_snap.tokens_today);
    ui_text(40, 210, line, 2);
    uint32_t scaled = g_snap.tokens_today > 1000000 ? 1000000 : g_snap.tokens_today;
    ui_progress(40, 232, 320, 14, scaled, 1000000);

    // Footer
    ui_text(40, 275, "zectrix note 4", 1);

    epd_full_refresh(ui_framebuf());
}

static void render_prompt(uint32_t hold_pct) {
    ui_clear(true);

    ui_fill(0, 0, 400, 60, false);
    ui_fill(10, 10, 380, 40, true);
    ui_text_center(200, 18, "PERMISSION NEEDED", 2);

    // Tool + hint
    char tool[48]; strncpy(tool, g_snap.prompt_tool[0] ? g_snap.prompt_tool : "?", sizeof(tool) - 1);
    tool[sizeof(tool) - 1] = 0;
    ui_text_center(200, 80, tool, 4);

    char hint[64]; strncpy(hint, g_snap.prompt_hint, sizeof(hint) - 1);
    hint[sizeof(hint) - 1] = 0;
    // naive truncation to fit ~32 chars at scale 2
    if (strlen(hint) > 32) hint[32] = 0;
    ui_text_center(200, 135, hint, 2);

    if (hold_pct == 0) {
        ui_text_center(200, 190, "Confirm = ALLOW", 2);
        ui_text_center(200, 220, "Hold confirm = DENY", 2);
    } else {
        ui_text_center(200, 195, "DENY", 4);
        ui_progress(40, 235, 320, 16, hold_pct, 100);
    }

    ui_text(40, 275, "button: front=confirm  sides=later", 1);
    epd_full_refresh(ui_framebuf());
}

// ---- Main loop --------------------------------------------------------------

// Very cheap: redraw only when something meaningfully changed. A simple
// hash of the visible fields does the job.
static uint32_t snap_hash(void) {
    uint32_t h = 2166136261u;
    #define MIX(x) do { h ^= (uint32_t)(x); h *= 16777619u; } while (0)
    MIX(g_snap.total); MIX(g_snap.running); MIX(g_snap.waiting);
    MIX(g_snap.tokens_today);
    for (const char *p = g_snap.msg; *p; p++) MIX(*p);
    for (const char *p = g_snap.prompt_id; *p; p++) MIX(*p);
    MIX(ble_nus_connected()); MIX(ble_nus_secure());
    #undef MIX
    return h;
}

extern "C" void app_main(void) {
    // NVS — required by BLE for bond storage.
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "boot");

    // Bring up display first so the splash shows early.
    ESP_ERROR_CHECK(epd_init());
    ui_init();
    ui_clear(true);
    ui_text_center(200, 120, "CLAUDE BUDDY", 3);
    ui_text_center(200, 170, "starting...", 2);
    epd_full_refresh(ui_framebuf());

    buttons_init();

    // Build our BLE name from the MAC's last 4 hex so duplicates are
    // distinguishable (same pattern as the CoreS3/AtomS3R ports).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char name[32];
    snprintf(name, sizeof(name), "Claude Note4 %02X%02X", mac[4], mac[5]);
    ble_nus_init(name);

    uint32_t last_hash = 0;
    uint32_t last_pk = 0;
    bool     had_prompt = false;

    while (true) {
        pump_ble();

        // Detect fresh prompt arrivals (for future: play chime).
        bool has_prompt = g_snap.prompt_id[0] != 0;
        bool new_prompt = has_prompt && strcmp(g_snap.prompt_id, g_last_prompt_id) != 0;
        if (new_prompt) {
            strncpy(g_last_prompt_id, g_snap.prompt_id, sizeof(g_last_prompt_id) - 1);
            g_last_prompt_id[sizeof(g_last_prompt_id) - 1] = 0;
            g_last_replied_id[0] = 0;
            ESP_LOGI(TAG, "[prompt] %s %s", g_snap.prompt_tool, g_snap.prompt_hint);
        }
        if (!has_prompt) { g_last_prompt_id[0] = 0; g_last_replied_id[0] = 0; }

        // Button handling
        btn_event_t e = buttons_poll();
        if (has_prompt) {
            if (e == BTN_CONFIRM_CLICK &&
                strcmp(g_snap.prompt_id, g_last_replied_id) != 0) {
                send_permission(g_snap.prompt_id, "once");
            } else if (e == BTN_CONFIRM_LONG &&
                       strcmp(g_snap.prompt_id, g_last_replied_id) != 0) {
                send_permission(g_snap.prompt_id, "deny");
            }
        }

        // Decide which screen to render
        uint32_t pk = ble_nus_passkey();
        bool redraw = false;
        if (pk != last_pk) { redraw = true; last_pk = pk; }

        uint32_t h = snap_hash();
        if (h != last_hash) { redraw = true; last_hash = h; }

        // While confirm is held during a prompt, pulse the deny bar every
        // ~500 ms — that's about as fast as e-paper partial refresh can
        // keep up with. Full partial-refresh support is a TODO.
        static uint32_t last_hold_redraw_ms = 0;
        uint32_t hold_ms = buttons_confirm_hold_ms();
        uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (has_prompt && buttons_confirm_held() &&
            (t_now - last_hold_redraw_ms) > 500) {
            last_hold_redraw_ms = t_now;
            redraw = true;
        }

        if (redraw) {
            if (pk) {
                render_pairing(pk);
            } else if (has_prompt) {
                uint32_t pct = 0;
                if (buttons_confirm_held()) {
                    pct = (hold_ms * 100) / 1200;  // 1200 = LONG_MIN_MS
                    if (pct > 100) pct = 100;
                }
                render_prompt(pct);
            } else {
                render_dashboard();
            }
        }

        had_prompt = has_prompt;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
