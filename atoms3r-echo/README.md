# Claude Desktop Buddy — M5Stack AtomS3R + Atom Echo Base

> 🚧 **Work in progress** — this port hasn't been built yet. Target hardware is `M5Stack AtomS3R` stacked on an `Atom Echo Base` for a 24×24 mm magnetic desk buddy with sound.

## Planned hardware

- **M5Stack AtomS3R** — ESP32-S3, 0.85″ 128×128 ST7789, BMI270 + BMM150 IMU, single big face button
- **Atom Echo Base** — NS4168 speaker amp + SPM1423 PDM mic over I2S; snaps under any Atom module
- Total footprint ~24 × 24 × 25 mm, magnetically attaches to a monitor edge

## Why this variant

| | CoreS3 | AtomS3R + Echo |
|---|---|---|
| Screen | 2.0″ 320×240 touch | 0.85″ 128×128, **no touch** |
| Input | Touch zones | Single button (short/long press) |
| Speaker | ✅ built-in | ✅ via Echo Base |
| Mic | ✅ | ✅ (Echo Base) |
| Form factor | Desktop device | **Magnetic monitor-edge clip** |
| Cost | ~¥500 | ~¥150 |

AtomS3R is the "clip-to-monitor-edge, glance at status, tap to approve" form of the same idea.

## Design notes

- **Input**: short press = Allow, long press (≥600 ms) = Deny. Long press shows a 3-dot countdown so it feels intentional.
- **Screen**: 128×128 is too small for the full CoreS3 layout. Plan is: big status colour fill (green/yellow/red) + single line of text (tool name when prompt, counters when idle).
- **Sound**: reuse the CoreS3 chime palette (prompt ding, ascending allow, low thud deny). Echo Base gives real audio output via M5.Speaker.
- **Pet**: either scaled-down 4×4-char ASCII cat, or dropped entirely in favour of pure colour-based state signalling — decide during build.
- **No touch, no VU meter**: UI is strictly read-only + 1-button.

## Code reuse from `cores3/`

Portable as-is:

- `ble_bridge.cpp` / `.h` — 100%, Arduino BLEDevice is identical on ESP32-S3
- JSON parsing + persona derivation from `main.cpp` — logic untouched, only the redraw functions change

Needs rewrite:

- UI drawing (128×128 layout)
- Input handling (touch → `M5.BtnA` short/long press)
- Possibly `cat_buddy.cpp` (or replace with colour-fill indicator)

## Status

Not started. Watch this space or open an issue if you want to help.
