# Claude Desktop Buddy — M5Stack CoreS3

A port of Anthropic's [Hardware Buddy protocol](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) to **M5Stack CoreS3**. The device mirrors Claude Desktop session state on its 320×240 touchscreen, lets you approve or deny tool-use permission prompts with a finger tap, and keeps you company with an 8-species ASCII pet.

The upstream firmware targets the M5StickC Plus (135×240, 3 physical buttons). This port reworks everything for CoreS3's larger touchscreen, uses `M5Unified`, and adds personality features that only make sense on the bigger device.

---

## What it does

![Layout overview](../docs/overview.png)

### Three screens

| Screen | When it shows | What's on it |
|---|---|---|
| **Pairing** | BLE is showing a 6-digit passkey | Full-width big digits, label "Pairing", instructions |
| **Idle** | Connected, no pending prompt | Pet + crown on the left, counters + tokens bar on the right, big status/greeting at bottom |
| **Prompt** | Claude is blocked on a permission decision | Same pet (attention pose), tool name + hint on the right, compact Allow/Deny touch zones at the bottom |

### Pet & personas

Four persona states, derived from the BLE snapshot. At a glance the **flower crown colour + pet pose** tells you what Claude is doing:

| Persona | Trigger | Pet animation | Crown |
|---|---|---|---|
| **Sleep** | Offline — no snapshot in 30 s | Curled up, Zzz particles | Wilted grey, static |
| **Idle** | Connected, nothing to do | Blinks, looks around, tail swish | Cherry pink + tender green, gentle bob |
| **Busy** | `running > 0` | Paw tap / intense stare / "..." | Orange + emerald, fast bob |
| **Attention** | Prompt present or `waiting > 0` | Alert, ears up, jitter, "!" marks | **Hidden** — pet movement alone signals urgency so the screen stays uncluttered |

### 8 pet species

Cycle through species by **double-tapping the left half of the screen**. The species you land on flashes its name big for 1.5 seconds so you know which one it is.

| Species | Body tint | Signature pose |
|---|---|---|
| **cat** | Warm orange | `/\_/\` `( o o )` |
| **owl** | Muted brown-grey | `/\  /\` `((O)(O))` |
| **duck** | Yellow | `<(o )___` on water `~~~~` |
| **penguin** | Cyan | `.---.` `( o>o )` `J   L` |
| **rabbit** | White | `(\_/)` long ears |
| **dragon** | Red | `/^\  /^\` wings + tail |
| **ghost** | White | `( -    - )` floating `~\`~~\`~` |
| **robot** | Silver | `[____]` `[ o o ]` |

The per-species crown x-offset is calibrated so the flowers sit on each pet's actual head midpoint.

### Header

- **`Claude Buddy`** title on the left
- **Clock** (`HH:MM`) in the middle once RTC is synced via `{"time":[...]}` from Claude Desktop
- **Mute icon** (red crossed-speaker) just left of the BLE status, when sound is muted
- **BLE status** on the right: `advert` (orange) / `paired` (yellow) / `secure` (green)

### Right panel (idle)

Stats columns:
- `running N` — sessions actively generating
- `waiting N` — sessions blocked on a permission prompt (yellow when non-zero)
- `total N` — total sessions
- Token counter for the day — colour shifts from soft grey → amber → red as you approach 1M
- Token progress bar, same colour tiers

### Bottom

A big status label or a personal greeting:

- `awaiting approval` — prompt active
- `waiting` — session blocked
- `working` — something running
- `waiting for claude` — BLE not secure yet
- `hi Name` — idle and Claude Desktop sent us the owner name via `{"cmd":"owner","name":"…"}`
- `idle` / `ready` fallback

---

## Sound

| Event | Audio |
|---|---|
| Prompt arrives — **per-tool chime** so you can tell what's being asked by ear | |
| &nbsp;&nbsp;`Bash` | Deep 540 Hz → 820 Hz ("caution") |
| &nbsp;&nbsp;`Write` | Firm 620 → 990 Hz |
| &nbsp;&nbsp;`Edit` / `MultiEdit` / `NotebookEdit` | Middle 740 → 1100 Hz |
| &nbsp;&nbsp;`Read` / `Glob` / `Grep` | Gentle 1050 → 1400 Hz |
| &nbsp;&nbsp;`WebFetch` / `WebSearch` | Airy 820 → 1240 Hz |
| &nbsp;&nbsp;other | Default 880 → 1320 Hz |
| **Allow** (touch right / short click) | Ascending triad E5 → A5 → E6 |
| **Deny** (touch left / long hold) | Descending thud D4 → G3 |

### Flip to mute

Hold the CoreS3 **face-down for ~600 ms** to toggle a global mute flag. A small red crossed-speaker icon appears in the header while muted. Leaving the device inverted only toggles once per flip session, so you can put it upside-down on the table without triggering repeated chirps. Un-muting plays a short 1200 Hz confirmation chirp.

All sound routines short-circuit when muted — visually the device still reacts to prompts, just silently.

---

## Hardware

- **M5Stack CoreS3** (ESP32-S3, 2.0" 320×240 ILI9342 touch, IMU, speaker, 8 MB PSRAM, 16 MB flash)
- USB-C data cable
- Mac or Windows running **Claude Cowork** or **Claude Code Desktop** with Hardware Buddy support

That's it. No extra wiring, no sensors.

---

## Building & flashing

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (works via the VS Code extension too)

### Flash

All PlatformIO commands must run from this `cores3/` directory:

```bash
cd cores3         # from the repo root
pio run -t upload
```

`platformio.ini` pins `upload_port = /dev/cu.usbmodem*` so PIO doesn't accidentally target a paired Bluetooth audio device on macOS.

First build fetches the toolchain and libs; subsequent builds are <10 s.

```
RAM:   ~16% (52 KB / 320 KB)
Flash: ~53% (1.1 MB / 2 MB)
```

### Monitor serial

```bash
pio device monitor
```

Exit with `Ctrl+C`. The device prints BLE events and accepts debug hotkeys (see below).

---

## Pairing with Claude Desktop

### First-time pairing

1. In Claude Desktop: **Help → Troubleshooting → Enable Developer Mode**
2. **Developer → Open Hardware Buddy…** — pairing window opens
3. Click **Connect**, pick `Claude CoreS3 XXXX` from the list
4. macOS / Windows shows a dialog asking for a 6-digit passkey
5. **The M5 displays the full 6-digit passkey on a dedicated "Pairing" screen** — type those digits into the desktop dialog
6. Window switches from `Disconnected` (grey dot) to `Connected` (green dot), preview thumbnail appears

Afterwards the device auto-reconnects whenever both sides are awake. Bonds persist across reboots via NVS.

### If pairing fails or loops

Stale bond records on either side will loop forever with cryptic `GATT_INSUF_ENCRYPTION` errors. Recover with a **strict two-sided reset**:

1. **Turn Bluetooth OFF** on your Mac (System Settings → Bluetooth)
2. In serial monitor, press `u` to clear device-side bonds, then press the Reset button on the side to restart the BLE stack
3. With Bluetooth still off, click **Forget** in the Hardware Buddy window; if `Claude CoreS3 XXXX` appears in macOS Bluetooth settings, forget it there too
4. Close the Hardware Buddy window
5. Turn Bluetooth back ON
6. Open Hardware Buddy, click **Connect**, re-pair as above

If it still fails: `sudo pkill bluetoothd` (or reboot the Mac).

---

## Testing the Allow/Deny flow

Claude Desktop only forwards a prompt over the Buddy channel when **it's actually blocked waiting for your decision**. Sessions with pre-approved tools never trigger a prompt. To force one:

1. Open a **new terminal** (avoids any per-project trust state)
2. `mkdir -p /tmp/buddy-test && cd /tmp/buddy-test`
3. `claude` (without `--dangerously-skip-permissions`)
4. Ask it to run a command that isn't in your global allow list, e.g. `whoami` or `date +%s`

You should see:

- Desktop: Allow/Deny dialog
- M5 screen: switches to red/green zones, tool name + hint appear
- Audio: per-tool chime plays (Bash deep, Read airy, etc.)
- Serial: `[rx] {"total":...,"prompt":{"id":"req_...","tool":"Bash","hint":"whoami"}}`

Tap **Allow** on M5 → desktop unblocks, command runs, ascending chime. Tap **Deny** → desktop gets "user denied", descending thud.

---

## Serial debug hotkeys

Useful for poking the UI without waiting on Claude. Type in `pio device monitor`:

| Key | Effect |
|---|---|
| `p` | Inject a fake permission prompt. Unique `debug_...` id each press. Lets you exercise the Allow/Deny UI without Claude. |
| `c` | Clear any active prompt state. |
| `r` | Bump `running` + `total` counters. Pet goes busy. |
| `i` | Reset counters + prompt → idle. |
| `u` | Erase all stored BLE bonds (NVS). You must re-pair afterwards. |
| `b` | Cycle buddy species (same as double-tap). |

Real BLE traffic overrides debug state on the next heartbeat. Debug prompts (id prefixed with `debug_`) auto-clear after you tap Allow/Deny; real prompts wait for the desktop's next snapshot to retract them.

Flip `DEBUG_LOG_RX` to 1 in `main.cpp` to see every inbound JSON line (spammy — only for protocol debugging).

---

## File layout

```
cores3/
├── platformio.ini      # m5stack-cores3 env, M5Unified + ArduinoJson
├── no_ota.csv          # single-app partition table
├── src/
│   ├── ble_bridge.cpp  # Nordic UART Service over BLE, LE Secure pairing
│   ├── ble_bridge.h    #   (vendored from upstream, unchanged)
│   ├── cat_buddy.cpp   # 8 ASCII pet species + spring flower crown
│   ├── cat_buddy.h
│   └── main.cpp        # JSON parsing, UI, touch handling, IMU wake+mute,
│                       #   sound palette, debug hotkeys
└── README.md           # (this file)
```

The BLE bridge is a verbatim copy from Anthropic's reference firmware — Arduino `BLEDevice` API is identical on ESP32 and ESP32-S3. Everything else is CoreS3-native.

---

## Architecture

### BLE protocol (inbound)

Desktop → device, newline-delimited JSON, max ~500 bytes/packet (MTU 517):

```json
{"time":[1776581000, 28800]}                        // RTC sync → clock
{"cmd":"owner","name":"Mango"}                      // greet by name
{"cmd":"status"}                                    // keepalive ping
{"total":3,"running":1,"waiting":1,                 // full snapshot
 "msg":"approve: Bash",
 "entries":["10:42 git push","10:41 yarn test"],
 "tokens_today":31200,
 "prompt":{"id":"req_abc123","tool":"Bash","hint":"rm -rf /tmp/foo"}}
```

We discriminate between "full snapshot" (has at least one of `total`/`running`/`waiting`/`msg`/`entries`/`prompt`) and "ping" (only `cmd:"status"` or `time` / `owner`). Only snapshots are allowed to clear prompt state — otherwise the ~1 Hz keepalive would dismiss the red/green screen before the user can tap.

### BLE protocol (outbound)

Device → desktop:

```json
{"cmd":"permission","id":"req_abc123","decision":"once"}
{"cmd":"permission","id":"req_abc123","decision":"deny"}
```

`id` must match the prompt's `id` exactly.

### Rendering

All UI draws into a **PSRAM-backed 320×240 M5Canvas**, then one `pushSprite` per frame. This eliminates the clear-then-redraw flicker you'd get by painting straight to the display. The pet lives in its own 150×108 child sprite pushed into the main canvas at y=28.

Pet canvas vertical layout (internal coordinates):
- `CROWN_Y_BASE = 14` — flower crown
- `POSE_Y_BASE = 28` — pose row 0 (rest of the pet)
- Per-species `crownXOffset()` moves the crown horizontally so the flowers sit on each pet's actual head midpoint.

Redraw rate: ~10 Hz so the pet + crown animate smoothly.

### Persona derivation

```cpp
if (waiting > 0 || prompt present)      → ATTENTION
if (running > 0)                        → BUSY
if (no snapshot in last 30 s)           → SLEEP
else                                    → IDLE
```

"Online" means "got a snapshot in the last 30 s", not "BLE is secure right now" — macOS cycles the physical link every ~20 s and we don't want the pet to flash to sleep each time.

### IMU — wake + mute

One poll loop reads accelerometer magnitude and z-axis:

- **Wake-on-motion**: magnitude delta > 0.08 g → brightness 200 for 30 s; dims to 40 after stillness. Bypassed while a prompt is pending.
- **Flip-to-mute**: accel.z < −0.7 g held for 600 ms → toggle the global mute flag. Latched per flip so leaving the device inverted doesn't retrigger.

### Non-ASCII filtering

The built-in GFX font doesn't render Chinese, emoji, etc. Any transcript text from the desktop goes through `asciiSanitize()` which replaces non-printable / non-ASCII runs with `?`. The entries panel is off by default (`SHOW_ENTRIES = false`) — a big-font status label is used instead. Flip the flag to see transcript entries.

---

## Known limitations & gotchas

- **BLE jitters on macOS** when you have many paired Bluetooth audio devices (AirPods, Bose, etc.). Macs cycle the radio and the CoreS3 link gets dropped for ~1 s every 20-30 s. Functionally harmless (reconnects automatically) — if you need rock-solid uptime, close audio devices during Claude sessions.
- **Prompt forwarding depends on the desktop app state.** If you're running `claude` in a terminal that was started with `--dangerously-skip-permissions` (or the project's `.claude/settings.local.json` pre-approves the tool), no prompt is generated and the M5 won't show anything — nothing is broken, Claude just isn't asking.
- **ASCII-only display.** No Chinese / emoji in the entries panel; they're replaced with `?`. If you need CJK, wire in `M5GFX`'s `efont_cn` (adds ~1 MB flash).
- **No voice approval.** An earlier clap-detector prototype triggered I2S register collisions between M5Unified's speaker and PDM mic. Removed. If you want real voice commands, the clean path is a raw `esp-idf` I2S PDM channel bypassing `M5.Mic`, or `esp-sr` for keyword spotting.
- **Species selection is not persisted.** Every boot starts on `cat`. Add NVS storage for `g_kind` if you want stickiness across reboots.

---

## Customization ideas

- **More species** — upstream has 18 (axolotl, blob, cactus, capybara, chonk, goose, mushroom, octopus, snail, turtle are the 10 not ported here). Same 5-row/12-col ASCII format, drop new namespaces into `cat_buddy.cpp`.
- **Richer persona states** — upstream also has celebrate / heart / dizzy states. Extend the `Persona` enum and route `snap.recentlyCompleted` or similar triggers to them.
- **Louder / quieter sounds** — `M5.Speaker.setVolume(128)` in `setup()`, range 0-255.
- **Different idle timeout** — `IDLE_DIM_MS` in `main.cpp`, default 30000.
- **Save selected pet** — write `buddyGetKind()` to NVS whenever it changes, read it in `setup()`.
- **Hats / eyes / rarity** — the upstream Claude desktop app reportedly has 5 rarities × 6 eye styles × 8 hats × 1% shiny. The hardware protocol doesn't carry this info so it would be purely cosmetic here. Not implemented — the current design prioritises glanceable state over collectibility.

---

## Credits

- BLE bridge (`ble_bridge.cpp/h`) by Anthropic, MIT-licensed, copied verbatim from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy).
- Pet ASCII art adapted from the same repo's `src/buddies/*.cpp`.
- Everything else in this repo: MIT.

---

## License

MIT. See `LICENSE` if present; otherwise treat this as public-domain-ish — do what you want.
