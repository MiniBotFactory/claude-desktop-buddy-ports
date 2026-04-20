# Claude Desktop Buddy — M5Stack AtomS3R + Atom Echo Base

A tiny magnetic clip-to-your-monitor companion for Claude Desktop. 24×24 mm device, 128×128 display used as a coloured status lamp, single-button Allow/Deny, full audio feedback via the Echo Base speaker.

> 🧪 **v0.1 — compiles, not yet hardware-validated.** Logic is ported from the CoreS3 build; the display/input layer is new. If you build it and something doesn't work, open an issue.

---

## What you need

- [**M5Stack AtomS3R**](https://shop.m5stack.com/products/atoms3r-development-kit) — ESP32-S3, 0.85″ 128×128, BMI270 + BMM150 IMU, single face button
- [**Atom Echo Base**](https://shop.m5stack.com/products/atom-echo-smart-speaker-base) — NS4168 speaker amp + SPM1423 PDM mic over I2S. Snaps under any Atom module.
- USB-C data cable
- Claude Cowork or Claude Code Desktop with Hardware Buddy support

Stack the AtomS3R on top of the Echo Base. Done.

---

## Build & flash

From this directory:

```bash
cd atoms3r-echo      # from the repo root
pio run -t upload
pio device monitor
```

Expected boot log:
```
[buddy] boot (AtomS3R+Echo)
[ble] advertising as 'Claude AtomS3R XXXX'
```

---

## Pairing

Same flow as the CoreS3 port — see [`../cores3/README.md`](../cores3/README.md#pairing-with-claude-desktop) for the full walk-through. On M5 the 6-digit passkey appears during pairing; type it into the Hardware Buddy dialog.

If anything gets stuck, press `u` in the serial monitor to clear stored bonds, then re-pair.

---

## UI model

The 128×128 screen is used as a **state lamp**, not a dashboard. Glance-from-2-metres is the design target.

### Idle states (fills the whole screen)

| Persona | Background | Centre text | Trigger |
|---|---|---|---|
| **Sleep** | Dim slate | `zZ` | No snapshot in 30 s |
| **Idle** | Soft green | `<tokens>K` today | Connected, nothing running |
| **Busy** | Amber | `R:<running count>` | Claude is generating |
| **Attention** | **Red pulse** | `!` | `waiting > 0` |

Top-left has a small text label (`ready` / `working` / `offline`). Top-right corner has a BLE-state dot (orange = advertising, yellow = paired, green = secure). If anything is `waiting`, a yellow `W:<n>` appears at the bottom.

### Prompt screen (fills the whole screen when a permission is pending)

```
┌──────────────┐
│    BASH      │   ← big tool name
│   whoami     │   ← hint, truncated
│              │
│  Tap: Allow  │
│  Hold 2s:Deny│
│              │
└──────────────┘
```

Background pulses red to grab attention. Tool name truncates at 8 chars, hint at 16.

### Deny countdown

While the button is held, the bottom half switches to:

```
┌──────────────┐
│    BASH      │
│   whoami     │
│              │
│    DENY      │   ← big
│  ▓▓▓▓░░░░    │   ← fills over 2 seconds
└──────────────┘
```

A short tick sound plays at the 1-second halfway mark. Release the button before 2 s to cancel. Hold through 2 s to confirm Deny.

---

## Interaction

Single button (`M5.BtnA`), three behaviours:

| Press pattern | Action |
|---|---|
| **Short click** (< 500 ms) | Allow the current prompt |
| **Hold for 2 seconds** | Deny (with countdown bar + tick at 1 s) |
| **Press, release mid-hold** | Cancel — no action sent |

Design rationale: Allow is the common case, so it's the fast path. Deny is deliberate (2-second hold) to avoid accidental rejection when you fumble the button. The countdown gives a visible cancel window.

Outside of a prompt, the button does nothing in v0.1. (Future use: cycle status pages, toggle mute, etc.)

---

## Audio

Echo Base routes `M5.Speaker` through its NS4168 amp, so the CoreS3 chime palette works here:

| Event | Sound |
|---|---|
| Prompt arrives | Two-tone chime (A5 → E6) |
| Allow | Ascending triad (E5 → A5 → E6) |
| Deny | Descending thud (D4 → G3) |
| Hold 1 s mark (deny halfway) | Short A4 tick |

Volume set to 180/255 in `setup()`; tweak in `main.cpp` if too loud.

---

## Serial debug hotkeys

Same as CoreS3:

| Key | Effect |
|---|---|
| `p` | Inject a fake prompt. Unique debug id each press. |
| `c` | Clear active prompt. |
| `r` | Bump `running` + `total`. |
| `i` | Reset to idle. |
| `u` | Erase BLE bonds — required before a fresh re-pair. |

---

## What's not in v0.1

- **No pet/animation.** The screen is too small for a readable ASCII cat; pure colour state is more effective. Could be added later as an 8×8-char sprite.
- **No mic use.** Echo Base has a mic but the CoreS3 experiments showed M5Unified's speaker + mic sharing I2S state ends in tears. If voice approval ever ships, it'll use raw `esp-idf` I2S PDM, bypassing M5.Mic.
- **No IMU wake.** Magnetic-clipped to a monitor edge, the device barely moves. v0.1 runs at a fixed medium brightness (120/255) which is visible but easy on the eyes.
- **No owner greeting.** Decided later — easy to add by hooking `{"cmd":"owner","name":"..."}` in `applyJson`.

---

## File layout

```
atoms3r-echo/
├── platformio.ini      # m5stack-atoms3 env, M5Unified + ArduinoJson
├── no_ota.csv          # single-app partition table
├── src/
│   ├── ble_bridge.cpp  # Nordic UART BLE (copy of cores3/src/ble_bridge.cpp)
│   ├── ble_bridge.h
│   └── main.cpp        # UI + button + state — the only AtomS3R-specific file
└── README.md           # (this file)
```

`ble_bridge.*` is an exact copy of the CoreS3 version. If you patch one, copy the patch over.

---

## Known gotchas

- **The Echo Base stacks UNDER the AtomS3R** (not on top). Some users flip them by accident; check before flashing.
- **Board ID in PlatformIO is `m5stack-atoms3`**, not `m5stack-atoms3r`. M5Unified auto-detects the `R` variant at `M5.begin()` time.
- **If `[cfg.external_speaker.atomic_echo = true]` doesn't compile**, your M5Unified version is too old. Upgrade to `M5Unified @ ^0.2.0` (already pinned in platformio.ini).
- **BLE jitters on macOS with many paired audio devices** — same as CoreS3, reconnects automatically, functionally harmless.
