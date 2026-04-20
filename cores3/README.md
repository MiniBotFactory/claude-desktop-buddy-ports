# Claude Desktop Buddy — M5Stack CoreS3

A port of Anthropic's [Hardware Buddy protocol](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) to **M5Stack CoreS3**. The device mirrors Claude Desktop session state on its 320×240 touchscreen and lets you approve or deny tool-use permission prompts with a finger tap — no need to context-switch back to the laptop.

The upstream firmware targets the M5StickC Plus (135×240, 3 physical buttons). This port reworks the UI for CoreS3's larger touchscreen, uses `M5Unified` instead of `M5StickCPlus`, and adds features that only make sense on the bigger device (pet + flower crown that shows Claude's state at a glance).

---

## What it does

![Layout overview](docs/overview.png) <!-- optional, add a photo later -->

### Three screens

| Screen | When it shows | What's on it |
|---|---|---|
| **Pairing** | 6-digit passkey is active | Full-width big digits, label "Pairing", instruction line |
| **Idle** | Connected, no pending prompt | Cat + flower crown on the left, counters + tokens bar on the right, big status label at bottom |
| **Prompt** | Claude is blocked on a permission decision | Same cat (with urgent red crown), tool name + hint on the right, compact Allow/Deny touch zones at the bottom |

### Pet personas

Four states, derived from the snapshot fields. At a glance the **flower crown colour** tells you Claude's state:

| Persona | Trigger | Cat animation | Crown |
|---|---|---|---|
| **Sleep** | Offline (no snapshot in 30s) | Curled up, Zzz particles | Wilted grey, static |
| **Idle** | Connected, nothing to do | Blinks, looks around, tail swishes | Cherry pink + tender green, gentle bob |
| **Busy** | `running > 0` | Paw tap, intense stare, "..." | Orange + emerald, fast bob |
| **Attention** | Prompt present or `waiting > 0` | Alert, ears up, bouncing, "!" marks | Vivid red + dark green, high-frequency flicker |

---

## Hardware

- **M5Stack CoreS3** (ESP32-S3, 2.0" 320×240 ILI9342 touch, IMU, speaker, 8MB PSRAM, 16MB flash)
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

On first run flash will fetch the toolchains and libs; subsequent builds are <10s.

```
RAM:   16.0% (52 KB / 320 KB)
Flash: 52.5% (1.1 MB / 2 MB)
```

### Monitor serial

```bash
pio device monitor
```

Exit with `Ctrl+C`. Useful because the device prints BLE events and accepts debug hotkeys (see below).

---

## Pairing with Claude Desktop

### First-time pairing

1. In Claude Desktop: **Help → Troubleshooting → Enable Developer Mode**
2. **Developer → Open Hardware Buddy…** — pairing window opens
3. Click **Connect**, pick `Claude CoreS3 XXXX` from the list
4. macOS / Windows will show a dialog asking for a 6-digit passkey
5. **The M5 will display the full 6-digit passkey on a dedicated "Pairing" screen**. Type those digits into the desktop dialog
6. Window should switch from `Disconnected` (grey dot) to `Connected` (green dot), preview thumbnail appears

Afterwards the device auto-reconnects whenever both sides are awake.

### If pairing fails or cycles

Stale bond records on either side will loop forever with cryptic `GATT_INSUF_ENCRYPTION` and `bta_dm_set_encryption` errors. Recover with a **strict two-sided reset**:

1. **Turn Bluetooth OFF** on your Mac (System Settings → Bluetooth)
2. On device: press `u` in the serial monitor to clear bonds, then press the Reset button on the side to force-restart the BLE stack
3. Still with Bluetooth off: click **Forget** in the Hardware Buddy window; if `Claude CoreS3 XXXX` appears in macOS Bluetooth settings, forget it there too
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
4. Ask it to run a command that isn't in your global allow list, e.g. `请运行 date +%s` or `whoami`

You should see:

- Desktop: Allow/Deny dialog
- M5 screen: switches to red/green zones, tool name and hint appear
- Serial: `[rx] {"total":...,"prompt":{"id":"req_...","tool":"Bash","hint":"..."}}`

Tap **Allow** on M5 → desktop unblocks, command runs, plays ascending chime. Tap **Deny** → desktop gets "user denied", plays low thud.

---

## Serial debug hotkeys

Useful when the desktop side isn't cooperating and you just want to poke the UI. Type in `pio device monitor`:

| Key | Effect |
|---|---|
| `p` | Inject a fake permission prompt. Generates a unique `debug_...` id each press. Lets you exercise the Allow/Deny UI without Claude Desktop. |
| `c` | Clear any active prompt state. |
| `r` | Bump `running` + `total` counters. Cat goes busy. |
| `i` | Reset counters + prompt → idle. |
| `u` | Erase all stored BLE bonds (NVS). After this, you must re-pair. Prints `[dbg] bonds cleared`. |

Real BLE traffic overrides debug state on the next heartbeat. Debug prompts (id prefixed with `debug_`) auto-clear after you tap Allow/Deny; real prompts wait for the desktop's next snapshot to retract them, which matches the protocol.

To see every inbound JSON line (very noisy, only for protocol debugging):

```cpp
// in src/main.cpp
#define DEBUG_LOG_RX 1
```

---

## File layout

```
cores3/
├── platformio.ini      # m5stack-cores3 env, M5Unified + ArduinoJson
├── no_ota.csv          # single-app partition table
├── src/
│   ├── ble_bridge.cpp  # Nordic UART Service over BLE, LE Secure pairing
│   ├── ble_bridge.h    #   (vendored from upstream, unchanged)
│   ├── cat_buddy.cpp   # ASCII-art cat animation + flower crown
│   ├── cat_buddy.h     #
│   └── main.cpp        # JSON parsing, UI, touch handling, IMU wake
└── README.md           # (this file)
```

(The top level of the repo holds one subdirectory per supported device —
see `../README.md` for the project-level overview.)

The BLE bridge is a verbatim copy from Anthropic's reference firmware — Arduino `BLEDevice` API is identical on ESP32 and ESP32-S3, so no changes were needed. Everything else is CoreS3-native.

---

## Architecture

### BLE protocol (inbound)

Desktop → device, newline-delimited JSON, max ~500 bytes/packet (MTU 517):

```json
{"time":[1776581000, 28800]}                        // RTC sync
{"cmd":"owner","name":"Mango"}                      // greet with user name
{"cmd":"status"}                                     // keepalive ping
{"total":3,"running":1,"waiting":1,                  // full snapshot
 "msg":"approve: Bash",
 "entries":["10:42 git push","10:41 yarn test"],
 "tokens_today":31200,
 "prompt":{"id":"req_abc123","tool":"Bash","hint":"rm -rf /tmp/foo"}}
```

We discriminate between "full snapshot" (has at least one of `total`/`running`/`waiting`/`msg`/`entries`/`prompt`) and "ping" (only `cmd:"status"` or `time`). Only snapshots are allowed to clear prompt state — otherwise the ~1 Hz keepalive would dismiss the red/green screen before the user can tap.

### BLE protocol (outbound)

Device → desktop:

```json
{"cmd":"permission","id":"req_abc123","decision":"once"}
{"cmd":"permission","id":"req_abc123","decision":"deny"}
```

`id` must match the prompt's `id` exactly. The upstream protocol defines other commands (`cmd:"status"` response, etc.) — we don't send them; they're optional.

### Rendering

All UI draws into a **PSRAM-backed 320×240 M5Canvas**, then one `pushSprite` per frame. This eliminates the clear-then-redraw flicker you get by painting straight to the display. The cat lives in its own 150×96 child sprite that pushes into the main canvas.

Redraw rate: ~10 Hz (every 100 ms) to animate the pet and crown smoothly.

### Persona derivation

```cpp
if (waiting > 0 || prompt present)      → ATTENTION
if (running > 0)                        → BUSY
if (no snapshot in last 30s)            → SLEEP
else                                    → IDLE
```

"Online" means "got a snapshot in the last 30s", not "BLE is secure right now" — macOS cycles the physical link every ~20s and we don't want the cat to flash to sleep each time.

### Non-ASCII filtering

The built-in GFX font doesn't render Chinese, emoji, etc. Any transcript text from the desktop goes through `asciiSanitize()` which replaces non-printable / non-ASCII runs with a single `?`. The entries panel is off by default (`SHOW_ENTRIES = false`) — a big-font status label is used instead, which is always ASCII. Flip the flag to see transcript entries.

---

## Known limitations & gotchas

- **BLE jitters on macOS** when you have many paired Bluetooth audio devices (AirPods, Bose, etc.). Macs cycle the radio and the CoreS3 link gets dropped for ~1s every 20-30s. Functionally harmless (reconnects automatically) — if you need rock-solid uptime, close audio devices during Claude sessions.
- **Prompt forwarding depends on the desktop app state.** If you're running `claude` in a terminal that was started with `--dangerously-skip-permissions` (or the project's `.claude/settings.local.json` pre-approves the tool), no prompt is generated and the M5 won't show anything — nothing is broken, Claude just isn't asking.
- **ASCII-only display.** No Chinese / emoji in the entries panel; they're replaced with `?`. If you need CJK, wire in `M5GFX`'s `efont_cn` (adds ~1 MB flash).
- **IMU wake uses accelerometer only.** Subtle wrist movement wakes it; sitting perfectly still at the desk will let it dim after 30s. A real "presence detector" would need the camera (disabled in this project because of I2C bus conflicts with M5Unified — see git history).
- **No voice approval.** An earlier clap-detector prototype triggered I2S register collisions between M5Unified's speaker and PDM mic. Removed. If you want real voice commands, the clean path is a raw `esp-idf` I2S PDM channel bypassing `M5.Mic`, or `esp-sr` for keyword spotting.

---

## Customization ideas

- **Different cat poses / add celebrate / heart / dizzy states** — upstream has these in `reference/src/buddies/` using the same 5-row ASCII format. Copy the pose arrays into `cat_buddy.cpp`, extend `Persona` enum.
- **Other animals** — 18 species exist upstream (owl, duck, penguin, dragon, etc.), all in the same format. Rename `cat_buddy.cpp` → `pet_buddy.cpp`, swap pose arrays.
- **Louder / quieter sounds** — `M5.Speaker.setVolume(128)` in `setup()`, 0-255.
- **Different idle timeout** — `IDLE_DIM_MS` constant, default 30000.
- **Different passkey screen** — `drawPairingScreen()` in `main.cpp`, uses font 7 (7-segment). Tweak to taste.

---

## Credits

- BLE bridge (`ble_bridge.cpp/h`) by Anthropic, MIT-licensed, copied verbatim from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy).
- Cat ASCII art adapted from the cat buddy in the same repo (`reference/src/buddies/cat.cpp`).
- Everything else in this repo: MIT.

---

## License

MIT. See `LICENSE` if present; otherwise treat this as public-domain-ish — do what you want.
