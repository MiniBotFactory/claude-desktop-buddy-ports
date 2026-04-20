# Claude Desktop Buddy — ZecTrix Note 4 (4.2″ e-paper)

The e-paper variant of the Hardware Buddy. Turns a [ZecTrix Note 4](https://wiki.zectrix.com/zh/hardware/intro) "AI 便利贴" into a monochrome dashboard that mirrors Claude Desktop session state and takes Allow/Deny decisions with the device's physical buttons.

> ✅ **v0.1.3 — hardware-verified.** Pairing (6-digit passkey on e-paper), dashboard rendering, full prompt flow (short-click Allow / long-hold Deny) all working on a real Note 4. Known limitations remain in the [TODO list](#todo) — audio, partial refresh, and deep sleep are still unimplemented.

---

## What makes this port different

Unlike the CoreS3 (2" LCD) and AtomS3R (0.85" OLED-ish LCD) ports, Note 4 is **e-paper**:

- Monochrome, 400 × 300, SSD2683 panel
- Full refresh ≈ 1.5 s; no pretense at animation
- Stays visible when the ESP32 is deep-sleeping → eventually an all-day-on-a-charge buddy
- No touch, no vivid colour → UI is a static *dashboard* + "block-letter" prompt screen

Instead of a pet, the whole 4.2" is the status surface: big `READY / WORKING / ATTENTION` label + counters + token progress bar. When a permission prompt arrives, the screen repaints once to show tool + hint, and the front button approves / long-press denies.

## Hardware

- **ZecTrix Note 4** (ESP32-S3, 16 MB flash, 8 MB PSRAM, 400×300 e-paper, 2000 mAh, front voice button + 2 side buttons, USB-C)
- That's it. Built-in speaker and mic are *present* but not used in this v0.1.

---

## Build & flash

This port uses **ESP-IDF** via PlatformIO (the other ports use Arduino — Note 4 needs the raw IDF for NimBLE and the SSD2683 SPI driver).

> ⚠️ **Python 3.12 required.** ESP-IDF's `idf-component-manager` depends on `pydantic-core`, which has no pre-built wheels for Python 3.14 as of this writing. If `pio --version` says it's running on 3.13+, reinstall via pipx on 3.12:
> ```bash
> brew uninstall platformio
> pipx install --python /opt/homebrew/bin/python3.12 platformio
> ```

```bash
cd zectrix-note4
pio run -t upload
pio device monitor
```

First build will pull the ESP-IDF toolchain (~500 MB) + a Python venv with `pydantic`/`idf-component-manager`/etc. (~100 MB). Takes 3–5 minutes on a warm network. Subsequent builds under 30 s.

Expected size after link:
```
RAM:   8.2% (27 KB / 320 KB)
Flash: 14.0% (586 KB / 4 MB)
```

### Entering the bootloader

Note 4 should auto-reset into ROM bootloader via DTR/RTS on USB. If `pio run -t upload` says `Failed to connect`, try:

1. Hold the front button while plugging in USB
2. Run `pio run -t upload` again
3. After flash, tap the side power button to reboot into the new firmware

---

## Pairing with Claude Desktop

Same flow as the other ports — see [`../cores3/README.md`](../cores3/README.md#pairing-with-claude-desktop) for the full walk-through. On Note 4 the 6-digit passkey appears on its own dedicated full-width screen; you type it on the Hardware Buddy dialog.

If something gets stuck, clear bonds from serial (not implemented yet in v0.1 — see TODO).

---

## UI screens

### Boot splash
```
┌───────────────────────────────┐
│                               │
│       CLAUDE BUDDY            │
│        starting...            │
│                               │
└───────────────────────────────┘
```

### Pairing
```
┌───────────────────────────────┐
│ CLAUDE BUDDY                  │
│ ─────────────                 │
│        Pairing                │
│                               │
│        5 1 1 2 4 1            │
│                               │
│ Type this on the desktop      │
└───────────────────────────────┘
```

### Dashboard
```
┌───────────────────────────────┐
│ ▓▓CLAUDE BUDDY▓▓     secure   │
│                               │
│           READY               │
│                               │
│   running  1                  │
│   waiting  0                  │
│   total    3                  │
│                               │
│   254,893 tokens today        │
│   [█████░░░░░░░░░]            │
│                               │
│   zectrix note 4              │
└───────────────────────────────┘
```

### Prompt
```
┌───────────────────────────────┐
│ ▓▓PERMISSION NEEDED▓▓         │
│                               │
│            BASH               │
│                               │
│     whoami                    │
│                               │
│     Confirm = ALLOW           │
│     Hold confirm = DENY       │
└───────────────────────────────┘
```

While holding confirm, the bottom half turns into:
```
│            DENY               │
│    [█████░░░░░░░░░░░░░░]      │
```

Release before the bar fills to cancel (no action). Hold through to ≈1.2 s to deny.

---

## Input

| Action | Result |
|---|---|
| Front button, short click (< 500 ms) | Allow the active prompt |
| Front button, hold ≥ 1.2 s | Deny (with progress bar) |
| Side up button | (future) cycle dashboard pages |
| Side down button | (future) cycle dashboard pages |
| Side down, long press | (future) power off |

---

## File layout

```
zectrix-note4/
├── platformio.ini          # framework=espidf, target=esp32s3, 16 MB flash
├── sdkconfig.defaults      # BLE NimBLE, PSRAM oct 80 MHz, 1 kHz FreeRTOS
├── partitions.csv          # single-app 4 MB partition
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── config.h            # pins — copied from ZecTrix SDK
│   ├── main.cc             # app_main + state machine + render dispatch
│   ├── epd_panel.c/h       # SSD2683 driver (extracted from ZecTrix, no LVGL)
│   ├── ble_nus.c/h         # NimBLE Nordic UART — replaces Arduino BLEDevice
│   ├── ui_paint.c/h        # 1bpp framebuffer + 5×7 font
│   ├── buttons.c/h         # three-button HAL with short/long detection
│   └── proto.cc/h          # Hardware Buddy JSON (cJSON)
└── README.md               # (this file)
```

---

## What's extracted from ZecTrix's SDK

- **Pin map** (`config.h`) — verbatim
- **EPD init sequence** (SSD2683 OTP commands 0x00/0xE9, temperature-compensated refresh via 0xE0/0xE6/0xA5/0x10) — translated from `custom_lcd_display.cc` with LVGL stripped
- **1bpp → SSD2683 wire-format packing** (`pack_1bpp`) — verbatim

Nothing else is borrowed. No LVGL, no xiaozhi framework, no Audio codec driver (v0.1 skips audio). The rest is written fresh.

---

## Protocol

Identical wire protocol to all other ports — Nordic UART service, newline-JSON, `{"cmd":"permission", "id":"...", "decision":"once|deny"}` reply. See [`../cores3/README.md`](../cores3/README.md#protocol-cheat-sheet) or Anthropic's [REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).

---

## TODO

v0.1 is feature-minimal. The following are known gaps, roughly ordered by impact:

### Must-have for daily use

- [ ] **Hardware validation** — run on a real Note 4 and confirm:
  - EPD init + full refresh display correctly
  - BLE advertises and pairs with Claude Desktop
  - Buttons register short/long correctly
  - Dashboard + prompt + pairing screens render legibly
- [ ] **Serial debug hotkeys** (`p`/`c`/`r`/`i`/`u`) — same as CoreS3/AtomS3R ports, makes bring-up 10× faster without a real prompt
- [ ] **Non-ASCII sanitise on inbound strings** — Claude may send Chinese transcript; our 5×7 font doesn't render it

### Nice-to-have

- [ ] **Audio feedback via ES8311 codec** — prompt chime, Allow chime, Deny thud. Needs the I2S + I2C init sequence from ZecTrix's `audio/codecs/` (not yet ported)
- [ ] **Partial refresh** for dashboard tick updates (counter change shouldn't require a 1.5 s full flash). Reference impl is in `custom_lcd_display.cc::EPD_DisplayPart`
- [ ] **LED indicator** (if the hardware exposes one on the front) to signal "new prompt waiting" while the screen is slow to update
- [ ] **Page cycling** with up/down buttons — show last transcript line, today's tool call histogram, etc.

### Long-term

- [ ] **Deep sleep + timed wake** to achieve the device's uA-level idle current. Current firmware holds BLE permanently so battery is ~2 days. The trick: deep-sleep for 5 s, wake, reconnect BLE, fetch a snapshot, redraw if anything changed, repeat. Adds ~5 s latency to every prompt but extends battery life to weeks.
- [ ] **NFC hook** — Note 4 has an NFC reader. Tapping a specific tag could, e.g., toggle mute or re-pair.
- [ ] **RTC sync** from the `{"time":[...]}` frame → Note 4 PCF8563 → on-screen clock even when BLE is down

---

## Gotchas we've already hit

- **ZecTrix SDK is ESP-IDF + LVGL + xiaozhi framework.** We don't inherit any of that — this port uses raw IDF + NimBLE, and the EPD driver was extracted standalone. If ZecTrix releases a new display firmware, diff their `custom_lcd_display.cc` against our `epd_panel.c` to pick up any controller-level changes (temperature tables, init sequence).
- **SSD2683 is not SSD1683.** It uses a 2-bit-per-cell wire format (packed with `pack_1bpp`) even though the panel is monochrome. Don't just plug in a GxEPD2 driver.
- **BUSY is active-low.** Panel pulls BUSY low while processing a refresh; we poll at 5 ms intervals and wait for high.
- **BTN_CONFIRM shares GPIO 0 with BOOT pin.** Holding at boot enters ROM bootloader — don't hold front button during power-up unless you want to flash.
- **BTN_DOWN shares GPIO 18 with VBAT_PWR.** A real long-press down should eventually power the device off; we don't implement that gracefully yet.
