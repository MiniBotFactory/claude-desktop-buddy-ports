# Claude Desktop Buddy — multi-device ports

Open-source hardware companions for [Claude Desktop](https://claude.ai/download) that mirror session state and let you approve or deny tool-use permission prompts without reaching for the laptop. Implements Anthropic's [Hardware Buddy BLE protocol](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).

<p align="center">
  <img src="docs/overview.png" alt="CoreS3 buddy showing the permission screen" width="320">
</p>

---

## Supported devices

Each device gets its own subdirectory with a self-contained build.

| Folder | Hardware | Form factor | Status |
|---|---|---|---|
| [`cores3/`](cores3/) | **M5Stack CoreS3** — 2.0″ 320×240 touchscreen + speaker + IMU | Desktop device, ~¥500 | ✅ **v0.5 polished** — 8 species, clock, greeting, per-tool chimes, flip-to-mute |
| [`atoms3r-echo/`](atoms3r-echo/) | **M5Stack AtomS3R + Atom Echo Base** — 0.85″ 128×128, 1 button, speaker + mic | Magnetic monitor-edge clip, ~¥150 | ✅ **v0.1 verified** |
| [`zectrix-note4/`](zectrix-note4/) | **ZecTrix Note 4** — 4.2″ e-paper 400×300, 3 buttons, speaker + mic | Always-on desk dashboard | 🟡 **v0.2 partial (paused)** — BLE + EPD + buttons verified, audio WIP |

The focus going forward is the **M5 family** (CoreS3, AtomS3R) because M5Stack's hardware is open, schematics are public, PlatformIO board packages are official, and the Arduino library (`M5Unified`) hides most peripheral bring-up. ZecTrix Note 4 works for the core BLE + display + buttons path but bringing up its custom ES8311 audio path took longer than the feature was worth; that port is paused and anyone interested is welcome to pick it up.

Pick the matching folder, `cd` in, and follow the README there.

---

## CoreS3 at a glance (v0.5)

The CoreS3 build is the one that gets day-to-day polish. Things it does that the upstream reference firmware doesn't:

- **8 pet species** you cycle through by double-tapping the left half of the screen — cat, owl, duck, penguin, rabbit, dragon, ghost, robot. The switched-to species flashes its name big for 1.5 s so you see what you landed on. Crown x-offset is calibrated per species so flowers sit on the actual head midpoint of each pet.
- **State-coloured flower crown** — wilted grey when sleeping, cherry pink when idle, bright orange when Claude is busy. Hidden during the ATTENTION state so the pet's own alert animation isn't fighting with the crown for attention.
- **Header clock** — `HH:MM` once the RTC is synced from a `{"time":[...]}` heartbeat.
- **Owner greeting** — when Claude sends `{"cmd":"owner","name":"Mango"}`, the idle status line becomes `hi Mango`.
- **Token counter colour matches budget tier** — soft grey / amber / red as you approach the 1 M daily cap, so budget pressure is visible without reading the number.
- **Per-tool prompt chime** — Bash deep 540→820 Hz, Edit/Write mid, Read/Glob/Grep airy 1050→1400 Hz, WebFetch middle 820→1240 Hz. You can tell what's being asked for by ear alone, without looking at the screen.
- **Flip-to-mute** — hold the CoreS3 face-down for 600 ms to toggle a global mute flag. A red crossed-speaker icon appears in the header. Latched per flip, so leaving it upside-down on the desk doesn't retrigger.
- **Pet + heartbeat + touch at 10 Hz in a PSRAM double buffer**, no redraw flicker.

See [`cores3/README.md`](cores3/README.md) for the full feature list, wire protocol, architecture notes, and debug hotkeys.

---

## How it works (shared across all ports)

### Protocol

Claude Desktop advertises a BLE Nordic UART Service. Any device that can:

1. Advertise NUS with a name starting with `Claude`
2. Accept LE Secure Connections pairing (6-digit passkey displayed on device)
3. Parse newline-delimited JSON heartbeats (session counters, token usage, optional `prompt` field)
4. Write back `{"cmd":"permission","id":"…","decision":"once|deny"}`

…is a valid buddy. See Anthropic's [REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) for the full wire protocol.

### Persona model

Every port derives the same four device states from the heartbeat, but renders them according to the hardware's strengths:

| State | Trigger | CoreS3 | AtomS3R | Note 4 |
|---|---|---|---|---|
| **Sleep** | No snapshot in 30 s | Pet curled up, Zzz, wilted crown | Screen off / dim grey | `OFFLINE` label |
| **Idle** | Connected, nothing to do | Pet blinking, pink crown, "hi Name" | Soft green fill | `READY` label |
| **Busy** | `running > 0` | Pet paw-tapping, orange crown | Yellow + `R:N` | `WORKING` label |
| **Attention** | Prompt or `waiting > 0` | Pet alert + jitter + "!", **no crown** (pet owns the alert) | Red fill + tool name | `ATTENTION` + tool + hint |

### Input mapping

| Port | Allow | Deny | Pet switch |
|---|---|---|---|
| CoreS3 | Tap right touch zone | Tap left touch zone | **Double-tap left half** to cycle species |
| AtomS3R | Short press (≤500 ms) | Long press (≥600 ms) | — |
| Note 4 | Front button short click (<2.5 s) | Front button hold ≥2.5 s | — |

### Audio feedback

| Port | Supported |
|---|---|
| CoreS3 | ✅ M5.Speaker on built-in AW88298 — **per-tool prompt chime** (Bash deep, Read airy, etc.), allow chord, deny thud, flip-to-mute |
| AtomS3R | ✅ via Atom Echo Base (NS4168) — same chime palette as CoreS3 |
| Note 4 | 🟡 ES8311 bring-up in progress, no confirmed audio yet |

---

## Project philosophy

- **One directory per device.** Each `<device>/` is a complete, independently-buildable project. No shared build system to wrestle with.
- **Shared code is duplicated, not linked.** The BLE bridge and JSON-parsing logic are copied into each device directory. 180 lines × 3 devices is cheaper than a build-system dance. If a bug is fixed, apply the patch in each directory.
- **UI is hardware-native.** Don't try to pretend AtomS3R has the same screen as CoreS3 — design to each device's strengths.
- **Frameworks chosen per device, not unified.** CoreS3 and AtomS3R use PlatformIO + Arduino (M5Unified handles drivers). Note 4 uses PlatformIO + ESP-IDF + NimBLE because its custom e-paper panel + ES8311 codec need raw IDF, and bringing the Arduino BLE stack into IDF would be worse than the duplication.

---

## Requirements

### Host tooling

- **PlatformIO Core** (or the VS Code extension) — `brew install platformio` on macOS.
- **Python 3.12.** The Note 4 ESP-IDF path pulls in `idf-component-manager` which depends on `pydantic-core`, and that package has no prebuilt wheels for Python 3.14 as of this writing. The Arduino ports (CoreS3, AtomS3R) are happy with any modern Python.
- **macOS, Linux, or WSL.** Every port has been exercised on macOS.

### Device tooling

- Just USB-C and a data cable. `pio run -t upload` drives all three devices.

---

## Adding a new device

Got an ESP32 variant that isn't listed? The protocol requirements are minimal:

- **Must have**: BLE (so no ESP32-S2), some way to display OR indicate state (screen, LEDs, e-ink), some way to approve/deny (buttons, touch, capacitive pad)
- **Nice to have**: speaker for audio feedback, IMU for motion wake, PSRAM for smooth double-buffered rendering

Suggested template:

```
<my-device>/
├── README.md              # hardware, pinout, build instructions, caveats
├── platformio.ini         # board + lib_deps
├── no_ota.csv             # partition table (optional)
└── src/                   # or main/ for ESP-IDF ports
    ├── ble_bridge.cpp     # copy from cores3/src/, unchanged
    ├── ble_bridge.h       # copy from cores3/src/, unchanged
    └── main.cpp           # your UI + input handling
```

PRs welcome. Keep ports focused on one device family; don't try to make one binary cover multiple boards via `#ifdef` — separate folders scale better.

---

## Releases

- **v0.5** — current. CoreS3 polish pass: 8 species with double-tap cycle, state-coloured flower crown (hidden in attention), header clock, owner greeting, per-tool chimes, flip-to-mute. AtomS3R + Note 4 unchanged from v0.4.
- **v0.4** — three ports, CoreS3 and AtomS3R hardware-verified end-to-end; Note 4 BLE + display + buttons verified, audio paused.
- **v0.3** — Note 4 scaffold added.
- **v0.2** — AtomS3R port added.
- **v0.1** — CoreS3 port first commit.

See `git tag -l` for the actual tags.

---

## Credits

- BLE bridge and wire protocol by [Anthropic](https://github.com/anthropics/claude-desktop-buddy), MIT.
- Cat ASCII art adapted from upstream [`cat.cpp`](https://github.com/anthropics/claude-desktop-buddy/blob/main/src/buddies/cat.cpp).
- ZecTrix Note 4 e-paper driver extracted from the vendor's public SDK ([wiki.zectrix.com/zh/software/opensource](https://wiki.zectrix.com/zh/software/opensource)), LVGL + task infrastructure stripped.
- Everything else: MIT, do as you wish.
