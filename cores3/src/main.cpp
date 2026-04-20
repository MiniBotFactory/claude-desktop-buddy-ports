// Claude Desktop Buddy — M5Stack CoreS3
// ============================================================================
// A hardware buddy that mirrors Claude Desktop session state on a 320x240
// touchscreen and lets the user approve or deny tool-use permission prompts
// with a finger tap. Speaks the Nordic UART BLE protocol documented at
//   https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md
//
// Features
//   * BLE Hardware Buddy protocol — LE Secure Connections pairing, NUS
//     service, newline-delimited JSON heartbeat & reply frames
//   * 320x240 UI, flicker-free PSRAM double buffer, three screens:
//       - pairing (6-digit passkey, full width)
//       - idle    (pet + counters + tokens + status)
//       - prompt  (pet + tool/hint + Allow/Deny touch zones)
//   * Pixel-ASCII cat buddy, 4 personas (sleep / idle / busy / attention)
//     with a spring flower crown whose colours track Claude's state
//   * Speaker tones for prompt arrival, Allow, Deny
//   * IMU motion wake — dims after 30s stillness, brightens on movement
//
// Debug: serial hotkeys `p`/`c`/`r`/`i`/`u` simulate prompts and clear bonds;
// see pumpSerialDebug() below. Normal RX traffic is NOT logged to keep the
// monitor readable; flip DEBUG_LOG_RX to 1 to see every inbound JSON line.
// ============================================================================

#include <Arduino.h>
#include <M5Unified.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "cat_buddy.h"

// ---------- snapshot state ---------------------------------------------------
struct Snapshot {
  uint8_t  total = 0;
  uint8_t  running = 0;
  uint8_t  waiting = 0;
  uint32_t tokensToday = 0;
  char     msg[48] = {0};
  char     entries[4][64] = {{0}};
  uint8_t  nEntries = 0;
  char     promptId[48] = {0};
  char     promptTool[24] = {0};
  char     promptHint[80] = {0};
  uint32_t lastUpdateMs = 0;
};
static Snapshot snap;
static char lastRepliedId[48] = {0};
static char lastPromptId[48]  = {0};

// Owner's first name as sent by Claude Desktop. Used to greet the user
// on the idle screen. Empty until the first {"cmd":"owner",...} arrives.
static char ownerName[32] = {0};

// Forward declaration — defined alongside handleIdleTouch further down.
// drawIdleScreen reads it to decide whether to overlay the buddy name.
extern uint32_t buddyNameFlashUntil;

// ---------- BLE RX line assembly --------------------------------------------
static char   lineBuf[1536];
static size_t lineLen = 0;

static void applyJson(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1].as<int32_t>();
    struct tm lt; gmtime_r(&local, &lt);
    M5.Rtc.setDateTime(lt);
    return;
  }

  // {"cmd":"owner","name":"Mango"} — keep the user's name for the greeting.
  const char* cmd = doc["cmd"];
  if (cmd && strcmp(cmd, "owner") == 0) {
    const char* nm = doc["name"];
    if (nm) {
      strncpy(ownerName, nm, sizeof(ownerName) - 1);
      ownerName[sizeof(ownerName) - 1] = 0;
    }
    return;
  }

  // Is this message a full heartbeat snapshot, or just a ping like
  // {"cmd":"status"} / {"time":[...]}?  Only heartbeats carry prompt state,
  // so a ping must NOT clear an active prompt.
  bool isSnapshot =
      doc["total"].is<uint8_t>()   || doc["running"].is<uint8_t>() ||
      doc["waiting"].is<uint8_t>() || !doc["entries"].isNull()    ||
      !doc["msg"].isNull()         || !doc["prompt"].isNull();

  if (!isSnapshot) return;  // ping / ack / unknown — ignore

  if (doc["total"].is<uint8_t>())         snap.total = doc["total"];
  if (doc["running"].is<uint8_t>())       snap.running = doc["running"];
  if (doc["waiting"].is<uint8_t>())       snap.waiting = doc["waiting"];
  if (doc["tokens_today"].is<uint32_t>()) snap.tokensToday = doc["tokens_today"];

  const char* m = doc["msg"];
  if (m) { strncpy(snap.msg, m, sizeof(snap.msg)-1); snap.msg[sizeof(snap.msg)-1]=0; }

  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 4) break;
      const char* s = v.as<const char*>();
      strncpy(snap.entries[n], s ? s : "", sizeof(snap.entries[0])-1);
      snap.entries[n][sizeof(snap.entries[0])-1] = 0;
      n++;
    }
    snap.nEntries = n;
  }

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(snap.promptId,   pid ? pid : "", sizeof(snap.promptId)-1);
    strncpy(snap.promptTool, pt  ? pt  : "", sizeof(snap.promptTool)-1);
    strncpy(snap.promptHint, ph  ? ph  : "", sizeof(snap.promptHint)-1);
  } else {
    // Full snapshot with no prompt → really no pending approval
    snap.promptId[0] = 0; snap.promptTool[0] = 0; snap.promptHint[0] = 0;
  }
  snap.lastUpdateMs = millis();
}

// Copy `src` into `dst`, replacing any non-ASCII byte (and control chars
// other than tab/space) with '?'. UTF-8 multi-byte sequences collapse to a
// run of '?' which we then dedupe so Chinese etc. becomes a single "?" per
// original character-ish region. Keeps the default GFX font happy.
static void asciiSanitize(const char* src, char* dst, size_t cap) {
  if (cap == 0) return;
  size_t o = 0;
  bool lastQ = false;
  for (const char* p = src; *p && o + 1 < cap; p++) {
    unsigned char b = (unsigned char)*p;
    if (b >= 0x20 && b < 0x7F) {
      dst[o++] = (char)b;
      lastQ = false;
    } else if (b == '\t') {
      dst[o++] = ' ';
      lastQ = false;
    } else {
      if (!lastQ) { dst[o++] = '?'; lastQ = true; }
    }
  }
  dst[o] = 0;
}

// Flip to 1 to dump every inbound JSON line to serial. Useful while
// diagnosing why a prompt isn't reaching the device; spammy otherwise.
#define DEBUG_LOG_RX 0

static void pumpBleLines() {
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        if (lineBuf[0] == '{') {
#if DEBUG_LOG_RX
          Serial.printf("[rx] %.160s%s\n", lineBuf, lineLen > 160 ? "..." : "");
#endif
          applyJson(lineBuf);
        }
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = (char)c;
    }
  }
}

static void sendPermission(const char* id, const char* decision) {
  char cmd[128];
  int n = snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}\n", id, decision);
  if (n > 0) bleWrite((const uint8_t*)cmd, n);
  Serial.printf("[tx] permission %s (%s)\n", decision, id);
  strncpy(lastRepliedId, id, sizeof(lastRepliedId)-1);
  lastRepliedId[sizeof(lastRepliedId)-1] = 0;
}

// ---------- sound ------------------------------------------------------------
// M5.Speaker (CoreS3: AW88298 over I2S). tone(hz, ms) is non-blocking.
//
// Per-tool chime palette: each tool category gets its own two-note motif
// so you can identify what's being requested by ear alone. The default
// ascending A5→E6 stays for unknown tools.
//
// A global mute flag (flip-to-mute via IMU, see imuPoll) short-circuits
// every sound function — when muted the device is still fully functional
// visually, just silent.

extern bool isMuted();   // defined below in the IMU section

static bool toolPrefix(const char *tool, const char *pfx) {
    if (!tool) return false;
    size_t n = strlen(pfx);
    for (size_t i = 0; i < n; i++) {
        char c = tool[i];
        if (c == 0) return false;
        // Case-insensitive for robustness against "Bash" vs "bash".
        char a = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        char b = (pfx[i] >= 'A' && pfx[i] <= 'Z') ? pfx[i] + 32 : pfx[i];
        if (a != b) return false;
    }
    return true;
}

static void soundPrompt() {
  if (isMuted()) return;
  // Pick tones based on snap.promptTool so different tools sound different.
  // Rule of thumb: dangerous tools get a LOW first tone (heavy attention),
  // read-only/benign tools get a HIGH first tone (light touch).
  const char *tool = snap.promptTool;
  uint16_t a = 880, b = 1320;                         // default (Bash / other)
  if      (toolPrefix(tool, "Bash"))       { a = 540;  b = 820; }   // deep, cautious
  else if (toolPrefix(tool, "Write"))      { a = 620;  b = 990; }   // firm
  else if (toolPrefix(tool, "Edit") ||
           toolPrefix(tool, "MultiEdit") ||
           toolPrefix(tool, "NotebookEdit")) { a = 740;  b = 1100; } // middle
  else if (toolPrefix(tool, "Read") ||
           toolPrefix(tool, "Glob") ||
           toolPrefix(tool, "Grep"))       { a = 1050; b = 1400; }  // gentle
  else if (toolPrefix(tool, "WebFetch") ||
           toolPrefix(tool, "WebSearch"))  { a = 820;  b = 1240; }  // airy
  M5.Speaker.tone(a, 80);  delay(90);
  M5.Speaker.tone(b, 140);
}

static void soundAllow() {
  if (isMuted()) return;
  M5.Speaker.tone(660, 70);   delay(80);
  M5.Speaker.tone(880, 70);   delay(80);
  M5.Speaker.tone(1320, 120);
}
static void soundDeny() {
  if (isMuted()) return;
  M5.Speaker.tone(300, 140);  delay(150);
  M5.Speaker.tone(200, 180);
}

// ---------- persona derivation ----------------------------------------------
// Map snapshot state to pet persona. "Online" means we got real data in the
// last 30s — using that instead of bleSecure() means transient BLE hiccups
// (macOS cycles us every ~20s) don't put the pet to sleep.
static Persona derivePersona() {
  uint32_t since = (snap.lastUpdateMs == 0) ? UINT32_MAX
                                            : (millis() - snap.lastUpdateMs);
  bool online = since < 30000;
  if (snap.waiting > 0 || snap.promptId[0])    return P_ATTENTION;
  if (snap.running > 0)                        return P_BUSY;
  if (!online)                                 return P_SLEEP;   // only sleep when truly offline
  return P_IDLE;                                                  // connected but nothing to do
}

// ---------- IMU motion wake + flip-to-mute ----------------------------------
// Screen brightens when the device is picked up, tilted, or bumped;
// dims after 30s of stillness. Bypassed while a prompt is pending.
//
// Flipping the CoreS3 face-down (accel Z < -0.7g for 600 ms) toggles a
// mute flag — all subsequent sound*() calls turn into no-ops. A small
// 🔇 indicator appears on the header when muted.
static uint32_t lastMotionMs = 0;
static float    lastAccMag   = 0;
static uint8_t  currentBrightness = 200;
static bool     muteFlag      = false;
static uint32_t faceDownStartMs = 0;

static constexpr uint32_t IDLE_DIM_MS    = 30000;
static constexpr uint8_t  BRIGHT_AWAKE   = 200;
static constexpr uint8_t  BRIGHT_DIM     = 40;
static constexpr uint32_t FLIP_HOLD_MS   = 600;      // must stay flipped this long
static constexpr float    FLIP_Z_THRESH  = -0.7f;    // face-down accel Z

bool isMuted() { return muteFlag; }

static void imuPoll() {
  if (M5.Imu.update()) {
    auto d = M5.Imu.getImuData();
    float mag = sqrtf(d.accel.x*d.accel.x + d.accel.y*d.accel.y + d.accel.z*d.accel.z);
    if (fabsf(mag - lastAccMag) > 0.08f) lastMotionMs = millis();
    lastAccMag = mag;

    // Flip-to-mute: accel Z is +1g face-up, -1g face-down. Each face-down
    // session may toggle mute exactly once (latched). User has to flip
    // back to face-up before another toggle can happen. Without the latch,
    // leaving the device upside down triggers a toggle every 2 seconds.
    static bool flipLatched = false;
    float z = d.accel.z;
    uint32_t now = millis();
    if (z < FLIP_Z_THRESH) {
      if (faceDownStartMs == 0) faceDownStartMs = now;
      if (!flipLatched && (now - faceDownStartMs) > FLIP_HOLD_MS) {
        muteFlag = !muteFlag;
        flipLatched = true;
        Serial.printf("[mute] %s (flip)\n", muteFlag ? "ON" : "OFF");
        // Chirp only when un-muting so the mute gesture itself stays silent.
        if (!muteFlag) M5.Speaker.tone(1200, 60);
      }
    } else {
      faceDownStartMs = 0;
      flipLatched = false;                          // ready for next flip
    }
  }

  bool hasPrompt = snap.promptId[0];
  uint32_t idleMs = millis() - lastMotionMs;
  uint8_t target = (hasPrompt || idleMs < IDLE_DIM_MS) ? BRIGHT_AWAKE : BRIGHT_DIM;
  if (target != currentBrightness) {
    M5.Display.setBrightness(target);
    currentBrightness = target;
  }
}

// ---------- UI ---------------------------------------------------------------
static constexpr int SW = 320, SH = 240;
static constexpr int HEADER_H = 24;

// Flip to `false` to hide the bottom entries panel entirely. Useful when
// Claude talks in a language the built-in GFX font can't render — the
// counters/pet/token bar are all ASCII-safe, entries are the only source
// of non-ASCII text.
static constexpr bool SHOW_ENTRIES = false;

// Full-screen off-screen canvas — all UI drawing goes here, then one
// pushSprite per frame. Eliminates the clear→redraw flicker.
static M5Canvas frame(&M5.Display);
static M5Canvas petCanvas(&frame);        // child sprite, pushed into frame
static bool frameReady = false;

static void uiInit() {
  M5.Display.setRotation(1);
  M5.Display.setBrightness(currentBrightness);
  M5.Display.fillScreen(TFT_BLACK);
  frame.setPsram(true);
  frame.setColorDepth(16);
  frame.createSprite(SW, SH);
  petCanvas.setPsram(false);
  petCanvas.setColorDepth(16);
  // Pet canvas is a bit taller than the pose itself (80 px) so the pet has
  // some breathing room above and below when centred; a pushSprite position
  // below header + a bit of margin makes the pet look vertically placed
  // instead of jammed against the header.
  petCanvas.createSprite(150, 108);
  frameReady = frame.getBuffer() != nullptr && petCanvas.getBuffer() != nullptr;
}

static void drawHeaderInto(M5Canvas& d) {
  d.fillRect(0, 0, SW, HEADER_H, 0x18C3);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(1);
  d.setTextFont(2);
  d.setTextDatum(middle_left);
  d.drawString("Claude Buddy", 8, HEADER_H/2);

  // Clock in the middle of the header. Only shown once the RTC has been
  // set from a {"time":[...]} frame — before that, the RTC holds its last
  // coin-cell value which would be off by hours. Cheap test: year >= 2024.
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year >= 2024) {
    char clk[8];
    snprintf(clk, sizeof(clk), "%02u:%02u", dt.time.hours, dt.time.minutes);
    d.setTextDatum(middle_center);
    d.setTextColor(0xE71Cu);         // soft cyan-grey
    d.drawString(clk, SW/2, HEADER_H/2);
  }

  d.setTextDatum(middle_right);
  // Mute badge just left of the BLE status. Small crossed speaker icon
  // made of rectangles — not Unicode (GFX font has no 🔇).
  int mxRight = SW - 8;
  if (isMuted()) {
    int mx = mxRight - 56;         // leave room for sig text
    int my = HEADER_H/2;
    d.fillRect(mx, my - 4, 3, 8, TFT_RED);            // speaker body
    d.fillTriangle(mx + 3, my - 6, mx + 3, my + 6, mx + 10, my, TFT_RED);
    d.drawLine(mx + 1, my - 8, mx + 12, my + 8, TFT_RED);  // slash
  }

  const char* sig; uint16_t col;
  if (!bleConnected())    { sig = "advert"; col = TFT_ORANGE; }
  else if (!bleSecure())  { sig = "paired"; col = TFT_YELLOW; }
  else                    { sig = "secure"; col = TFT_GREEN;  }
  d.setTextColor(col);
  d.drawString(sig, SW - 8, HEADER_H/2);
}

static void drawTokenBarInto(M5Canvas& d, int x, int y, int w, int h, uint32_t tokens) {
  uint32_t pct = tokens > 1000000 ? 100 : (tokens / 10000);
  d.drawRect(x, y, w, h, 0x52AA);
  d.fillRect(x+1, y+1, (w-2) * pct / 100, h-2,
             pct < 60 ? TFT_GREEN : pct < 85 ? TFT_YELLOW : TFT_RED);
}

// Dedicated pairing screen — shown when the BLE stack has an active
// 6-digit passkey to display. Gets the full 320px width so the last digit
// never falls off screen (the previous split layout truncated it at 5).
static void drawPairingScreen(uint32_t pk) {
  // Diagnostic — if this log appears in serial but the screen stays dark,
  // the issue is in the frame/sprite path, not the loop control flow.
  static uint32_t lastLogPk = 0;
  if (pk != lastLogPk) {
    Serial.printf("[ui] drawPairingScreen pk=%06lu frameReady=%d\n",
                  (unsigned long)pk, frameReady ? 1 : 0);
    lastLogPk = pk;
  }

  if (!frameReady) return;
  frame.fillSprite(TFT_BLACK);
  drawHeaderInto(frame);

  frame.setTextDatum(middle_center);
  frame.setTextFont(4);
  frame.setTextColor(0xFFE0);        // amber title
  frame.drawString("Pairing", SW/2, 56);

  char buf[8];
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)pk);

  // Use font 4 at textSize 2 instead of font 7 — font 7 is a 7-segment
  // style only available on some M5GFX builds; switching to a plain
  // scaled font is more portable and nearly as big.
  frame.setTextFont(4);
  frame.setTextSize(2);
  frame.setTextColor(TFT_CYAN);
  frame.drawString(buf, SW/2, 140);
  frame.setTextSize(1);

  frame.setTextFont(2);
  frame.setTextColor(0xBDF7);
  frame.drawString("Type this on the desktop dialog", SW/2, 210);

  frame.pushSprite(0, 0);
}

static void drawIdleScreen(Persona p, uint32_t now) {
  if (!frameReady) return;
  frame.fillSprite(TFT_BLACK);

  drawHeaderInto(frame);

  // Pet region: draw cat into its own sprite, then push at (4, 30)
  petCanvas.fillSprite(TFT_BLACK);
  catDrawFrame(&petCanvas, p, now);
  petCanvas.pushSprite(&frame, 4, 32);

  // Flash the buddy name briefly after the user double-taps to switch.
  if (millis() < buddyNameFlashUntil) {
    frame.setTextDatum(middle_center);
    frame.setTextFont(4);
    frame.setTextColor(TFT_WHITE, 0x18C3);          // white on header-grey
    const char *name = buddyKindName(buddyGetKind());
    int w = frame.textWidth(name) + 20;
    frame.fillRoundRect(80 - w/2, 60, w, 28, 6, 0x18C3);
    frame.drawString(name, 80, 74);
  }

  // Stats panel on the right (160..320)
  const int rx = 160, ry = 32;
  frame.setTextFont(2);
  frame.setTextSize(1);
  frame.setTextDatum(top_left);

  {
    char line[40];
    frame.setTextColor(TFT_WHITE);
    snprintf(line, sizeof(line), "running  %u", snap.running);
    frame.drawString(line, rx, ry + 0);
    frame.setTextColor(snap.waiting ? TFT_YELLOW : TFT_WHITE);
    snprintf(line, sizeof(line), "waiting  %u", snap.waiting);
    frame.drawString(line, rx, ry + 20);
    frame.setTextColor(TFT_WHITE);
    snprintf(line, sizeof(line), "total    %u", snap.total);
    frame.drawString(line, rx, ry + 40);

    // Colour the token count to match the bar tier — so you notice the
    // warning even without staring at the fill ratio. Thresholds match
    // drawTokenBarInto: <60% soft, 60-85% amber, >=85% red.
    uint32_t pct = snap.tokensToday > 1000000 ? 100 : (snap.tokensToday / 10000);
    uint16_t tokCol = (pct < 60) ? 0x9CD3u
                   : (pct < 85) ? TFT_YELLOW
                                : TFT_RED;
    frame.setTextColor(tokCol);
    snprintf(line, sizeof(line), "%lu tokens", (unsigned long)snap.tokensToday);
    frame.drawString(line, rx, ry + 65);
    drawTokenBarInto(frame, rx, ry + 85, SW - rx - 10, 10, snap.tokensToday);
  }

  // Divider
  frame.drawFastHLine(4, 140, SW - 8, 0x31A6);

  if (SHOW_ENTRIES) {
    frame.setTextColor(0xBDF7);
    frame.setTextFont(2);
    int y = 148;
    for (uint8_t i = 0; i < snap.nEntries && y < SH - 16; i++) {
      char buf[48];
      asciiSanitize(snap.entries[i], buf, sizeof(buf));
      frame.drawString(buf, 6, y);
      y += 18;
    }
    if (snap.nEntries == 0 && snap.msg[0]) {
      char buf[48];
      asciiSanitize(snap.msg, buf, sizeof(buf));
      frame.drawString(buf, 6, 148);
    }
  } else {
    // Big-font status line in the lower half. When we know the user's
    // name and nothing's actively happening, use a personalised greeting
    // instead of a bare "ready" / "idle".
    frame.setTextDatum(middle_center);
    frame.setTextFont(4);
    frame.setTextColor(0xBDF7);
    char buf[48];
    const char* status = buf;
    if (snap.promptId[0]) {
      status = "awaiting approval";
    } else if (snap.waiting) {
      status = "waiting";
    } else if (snap.running) {
      status = "working";
    } else if (!bleSecure()) {
      status = "waiting for claude";
    } else if (ownerName[0]) {
      snprintf(buf, sizeof(buf), "hi %s", ownerName);
    } else if (snap.total == 0) {
      status = "idle";
    } else {
      status = "ready";
    }
    frame.drawString(status, SW/2, 180);
  }

  if (!bleSecure()) {
    frame.setTextDatum(middle_center);
    frame.setTextFont(2);
    frame.setTextColor(0x7BEF);
    frame.drawString("Developer > Hardware Buddy", SW/2, SH - 14);
  }

  frame.pushSprite(0, 0);
}

// Prompt screen — keeps the pet visible on top, compact Allow/Deny strip
// at the bottom. The right panel reuses the stats area to show what the
// tool is and its hint, so the top half feels continuous with the idle
// screen (same pet, same header) and only the bottom third changes.
static constexpr int ZONE_TOP = 168;
static constexpr int ZONE_H   = 64;
static constexpr int DENY_X   = 8;
static constexpr int APPR_X   = 164;
static constexpr int ZONE_W   = 148;

static void drawPromptScreen(Persona p, uint32_t now) {
  if (!frameReady) return;
  frame.fillSprite(TFT_BLACK);
  drawHeaderInto(frame);

  // Pet on the left — same position as idle. Use ATTENTION persona so the
  // cat perks up and the crown turns urgent red.
  petCanvas.fillSprite(TFT_BLACK);
  catDrawFrame(&petCanvas, p, now);
  petCanvas.pushSprite(&frame, 4, 32);

  // Right panel (160..320, 30..140): tool name + hint, replacing stats
  const int rx = 160, ry = 36;
  frame.setTextDatum(top_left);
  frame.setTextFont(2);
  frame.setTextSize(1);
  frame.setTextColor(0xFBE0);                // amber label
  frame.drawString("permission", rx, ry);

  frame.setTextColor(TFT_WHITE);
  char toolBuf[32];
  asciiSanitize(snap.promptTool[0] ? snap.promptTool : "?", toolBuf, sizeof(toolBuf));
  char head[48];
  snprintf(head, sizeof(head), "%s", toolBuf);
  frame.setTextFont(4);                       // larger tool name
  frame.drawString(head, rx, ry + 16);

  frame.setTextFont(2);
  frame.setTextColor(0xBDF7);
  char hint[80];
  asciiSanitize(snap.promptHint, hint, sizeof(hint));
  // Soft-wrap hint into two lines at ~24 chars (font2 avg width ~8px)
  const int maxChars = 24;
  int hl = strlen(hint);
  if (hl <= maxChars) {
    frame.drawString(hint, rx, ry + 52);
  } else {
    char a[48]; char b[48];
    int split = maxChars;
    while (split > 12 && hint[split] != ' ' && hint[split] != '/' ) split--;
    if (split <= 12) split = maxChars;
    strncpy(a, hint, split); a[split] = 0;
    strncpy(b, hint + split, sizeof(b) - 1); b[sizeof(b)-1] = 0;
    frame.drawString(a, rx, ry + 52);
    frame.drawString(b, rx, ry + 70);
  }

  // Divider matches idle layout
  frame.drawFastHLine(4, 158, SW - 8, 0x31A6);

  // Compact Allow/Deny buttons
  frame.fillRoundRect(DENY_X, ZONE_TOP, ZONE_W, ZONE_H, 10, 0xA000);
  frame.fillRoundRect(APPR_X, ZONE_TOP, ZONE_W, ZONE_H, 10, 0x0480);

  frame.setTextDatum(middle_center);
  frame.setTextSize(2);
  frame.setTextColor(TFT_WHITE);
  frame.drawString("Deny",  DENY_X + ZONE_W/2, ZONE_TOP + ZONE_H/2);
  frame.drawString("Allow", APPR_X + ZONE_W/2, ZONE_TOP + ZONE_H/2);
  frame.setTextSize(1);

  frame.pushSprite(0, 0);
}

// Show the buddy's name large on screen for this many ms after the user
// switches species, so they know which one they landed on without pairing
// the touch to a list display. Non-static so the forward decl up top
// can bind (drawIdleScreen reads it).
uint32_t buddyNameFlashUntil = 0;

// Idle touch: double-tap the LEFT half of the screen (comfortably larger
// than the pet canvas itself — the pet is only ~150x108 but the user
// should be able to slap anywhere on the left side and have it register).
// Single taps are ignored so you can swipe past the pet without
// accidentally changing it.
static void handleIdleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;
  // Left half of the screen, below header (y>=24), above the divider +
  // some slop to catch finger drift.
  if (t.x > 180 || t.y < 24 || t.y > 150) return;

  static uint32_t lastTapMs = 0;
  uint32_t now = millis();
  if (lastTapMs != 0 && (now - lastTapMs) < 400) {
    // Second tap → cycle
    BuddyKind next = (BuddyKind)((buddyGetKind() + 1) % BUDDY_COUNT);
    buddySetKind(next);
    buddyNameFlashUntil = now + 1500;
    Serial.printf("[buddy] -> %s\n", buddyKindName(next));
    lastTapMs = 0;
  } else {
    lastTapMs = now;
  }
}

static bool handlePromptTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return false;
  if (snap.promptId[0] == 0) return false;
  if (strcmp(snap.promptId, lastRepliedId) == 0) return false;
  if (t.y < ZONE_TOP || t.y > ZONE_TOP + ZONE_H) return false;

  // For locally-injected debug prompts (id starts with "debug_") the
  // desktop isn't in the loop to retract the prompt on its next heartbeat,
  // so clear it ourselves after replying. Real prompts are left alone —
  // Claude's next snapshot will drop the `prompt` field.
  auto clearIfDebug = []() {
    if (strncmp(snap.promptId, "debug_", 6) == 0) {
      snap.promptId[0] = 0;
      snap.promptTool[0] = 0;
      snap.promptHint[0] = 0;
      snap.waiting = 0;
    }
  };

  if (t.x >= APPR_X && t.x <= APPR_X + ZONE_W) {
    sendPermission(snap.promptId, "once");
    soundAllow();
    M5.Display.fillRoundRect(APPR_X, ZONE_TOP, ZONE_W, ZONE_H, 12, TFT_GREEN);
    clearIfDebug();
    return true;
  }
  if (t.x >= DENY_X && t.x <= DENY_X + ZONE_W) {
    sendPermission(snap.promptId, "deny");
    soundDeny();
    M5.Display.fillRoundRect(DENY_X, ZONE_TOP, ZONE_W, ZONE_H, 12, TFT_RED);
    clearIfDebug();
    return true;
  }
  return false;
}

// ---------- setup / loop -----------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_imu = true;
  M5.begin(cfg);

  uiInit();

  M5.Speaker.setVolume(128);

  Serial.begin(115200);
  delay(50);
  Serial.println("\n[buddy] boot");

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char name[32];
  snprintf(name, sizeof(name), "Claude CoreS3 %02X%02X", mac[4], mac[5]);
  bleInit(name);
  lastMotionMs = millis();
}

// Serial debug hook — type one of these chars in `pio device monitor`:
//   p = fake an incoming permission prompt (so you can test sound + UI
//       without Claude Desktop)
//   c = clear the fake prompt
//   r = bump running counter (simulate a busy state)
//   i = reset to idle (all zeros)
// Real BLE traffic overrides these on the next heartbeat.
static void pumpSerialDebug() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 'p':
        // Unique id per press — avoids staleness in lastRepliedId tracking.
        snprintf(snap.promptId, sizeof(snap.promptId), "debug_%lu",
                 (unsigned long)millis());
        strcpy(snap.promptTool, "Bash");
        strcpy(snap.promptHint, "rm -rf /tmp/test");
        snap.waiting = 1;
        Serial.printf("[dbg] fake prompt id=%s\n", snap.promptId);
        break;
      case 'c':
        snap.promptId[0] = 0; snap.promptTool[0] = 0; snap.promptHint[0] = 0;
        snap.waiting = 0;
        Serial.println("[dbg] prompt cleared");
        break;
      case 'r':
        snap.running++; snap.total++;
        Serial.printf("[dbg] running=%u total=%u\n", snap.running, snap.total);
        break;
      case 'i':
        snap.running = snap.waiting = snap.total = 0;
        snap.promptId[0] = 0;
        Serial.println("[dbg] idle");
        break;
      case 'u':
        bleClearBonds();
        Serial.println("[dbg] bonds cleared — unpair on desktop and re-pair");
        break;
      case 'b': {
        BuddyKind next = (BuddyKind)((buddyGetKind() + 1) % BUDDY_COUNT);
        buddySetKind(next);
        Serial.printf("[dbg] buddy → %s\n", buddyKindName(next));
        break;
      }
    }
  }
}

void loop() {
  M5.update();
  pumpBleLines();
  pumpSerialDebug();
  imuPoll();

  bool hasPrompt = snap.promptId[0] != 0;
  bool newPrompt = hasPrompt && strcmp(snap.promptId, lastPromptId) != 0;
  if (newPrompt) {
    strncpy(lastPromptId, snap.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    lastRepliedId[0] = 0;   // fresh prompt → touch is armed again
    soundPrompt();
    lastMotionMs = millis();  // wake screen on prompt
  }
  if (!hasPrompt) { lastPromptId[0] = 0; lastRepliedId[0] = 0; }

  static uint32_t lastDraw = 0;
  static bool     lastHadPrompt = false;
  static uint32_t lastPk = 0;

  // Priority: pairing passkey > prompt > idle. Passkey is short-lived
  // and critical — user can't complete pairing without seeing all 6 digits.
  uint32_t pk = blePasskey();

  if (pk) {
    if (pk != lastPk || millis() - lastDraw > 500) {
      drawPairingScreen(pk);
      lastDraw = millis();
    }
  } else if (hasPrompt) {
    if (lastHadPrompt == false || millis() - lastDraw > 100) {
      drawPromptScreen(P_ATTENTION, millis());
      lastDraw = millis();
    }
    if (handlePromptTouch()) {
      M5.Display.display();
      delay(300);
    }
  } else {
    handleIdleTouch();
    if (lastHadPrompt || lastPk || millis() - lastDraw > 100) {
      drawIdleScreen(derivePersona(), millis());
      lastDraw = millis();
    }
  }
  lastHadPrompt = hasPrompt;
  lastPk = pk;

  delay(10);
}
