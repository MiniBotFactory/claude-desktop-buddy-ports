// Claude Desktop Buddy — M5Stack AtomS3R + Atom Echo Base (v0.1)
// ============================================================================
// Magnetic monitor-edge clip that mirrors Claude Desktop session state and
// takes Allow/Deny decisions via a single face button. 128x128 display is
// used as a "status lamp" — full-screen colour shows persona at a glance,
// a line of text gives the one useful detail.
//
// UI model
//   SLEEP     → dim grey fill,   "zZ"
//   IDLE      → green fill,      token count ("123K")
//   BUSY      → amber fill,      running count ("R:1")
//   ATTENTION → red pulse fill,  tool name  ("Bash")
//   (PROMPT   → red screen + tool + hint + instructions + countdown)
//
// Input
//   Short click (<300ms release)         → Allow
//   Hold 2000ms (with visible countdown) → Deny
//   Release mid-hold                     → cancel, no action
//
// Audio via Echo Base speaker: prompt ding, ascending Allow, descending Deny.
//
// BLE and JSON logic ports verbatim from the CoreS3 variant — the protocol
// is hardware-independent.
// ============================================================================

#include <Arduino.h>
#include <M5Unified.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"

// ---------- snapshot state ---------------------------------------------------
struct Snapshot {
  uint8_t  total = 0;
  uint8_t  running = 0;
  uint8_t  waiting = 0;
  uint32_t tokensToday = 0;
  char     msg[48] = {0};
  char     promptId[48] = {0};
  char     promptTool[24] = {0};
  char     promptHint[64] = {0};
  uint32_t lastUpdateMs = 0;
};
static Snapshot snap;
static char lastRepliedId[48] = {0};
static char lastPromptId[48]  = {0};

// ---------- persona ---------------------------------------------------------
enum Persona : uint8_t { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION };

static Persona derivePersona() {
  uint32_t since = (snap.lastUpdateMs == 0) ? UINT32_MAX
                                            : (millis() - snap.lastUpdateMs);
  bool online = since < 30000;
  if (snap.waiting > 0 || snap.promptId[0]) return P_ATTENTION;
  if (snap.running > 0)                     return P_BUSY;
  if (!online)                              return P_SLEEP;
  return P_IDLE;
}

// ---------- ASCII sanitise (strip non-ASCII for the built-in GFX font) ------
static void asciiSanitize(const char* src, char* dst, size_t cap) {
  if (cap == 0) return;
  size_t o = 0;
  bool lastQ = false;
  for (const char* p = src; *p && o + 1 < cap; p++) {
    unsigned char b = (unsigned char)*p;
    if (b >= 0x20 && b < 0x7F) { dst[o++] = (char)b; lastQ = false; }
    else if (b == '\t')        { dst[o++] = ' ';    lastQ = false; }
    else if (!lastQ)           { dst[o++] = '?';    lastQ = true;  }
  }
  dst[o] = 0;
}

// ---------- BLE line parsing ------------------------------------------------
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

  // Only clear prompt state for true snapshots; ignore pings like {"cmd":"status"}.
  bool isSnapshot =
      doc["total"].is<uint8_t>()   || doc["running"].is<uint8_t>() ||
      doc["waiting"].is<uint8_t>() || !doc["entries"].isNull()    ||
      !doc["msg"].isNull()         || !doc["prompt"].isNull();
  if (!isSnapshot) return;

  if (doc["total"].is<uint8_t>())         snap.total = doc["total"];
  if (doc["running"].is<uint8_t>())       snap.running = doc["running"];
  if (doc["waiting"].is<uint8_t>())       snap.waiting = doc["waiting"];
  if (doc["tokens_today"].is<uint32_t>()) snap.tokensToday = doc["tokens_today"];

  const char* m = doc["msg"];
  if (m) { strncpy(snap.msg, m, sizeof(snap.msg)-1); snap.msg[sizeof(snap.msg)-1]=0; }

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(snap.promptId,   pid ? pid : "", sizeof(snap.promptId)-1);
    strncpy(snap.promptTool, pt  ? pt  : "", sizeof(snap.promptTool)-1);
    strncpy(snap.promptHint, ph  ? ph  : "", sizeof(snap.promptHint)-1);
  } else {
    snap.promptId[0] = 0; snap.promptTool[0] = 0; snap.promptHint[0] = 0;
  }
  snap.lastUpdateMs = millis();
}

static void pumpBleLines() {
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        if (lineBuf[0] == '{') applyJson(lineBuf);
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

// ---------- sound -----------------------------------------------------------
static void soundPrompt() {
  M5.Speaker.tone(880, 80);   delay(90);
  M5.Speaker.tone(1320, 140);
}
static void soundAllow() {
  M5.Speaker.tone(660, 70);   delay(80);
  M5.Speaker.tone(880, 70);   delay(80);
  M5.Speaker.tone(1320, 120);
}
static void soundDeny() {
  M5.Speaker.tone(300, 140);  delay(150);
  M5.Speaker.tone(200, 180);
}
static void soundHoldTick() { M5.Speaker.tone(440, 30); }

// ---------- UI --------------------------------------------------------------
static constexpr int SW = 128, SH = 128;

// Persona → fill colour. Attention pulses via millis() modulation.
static uint16_t personaColor(Persona p, uint32_t now) {
  switch (p) {
    case P_SLEEP:     return 0x18C3;            // dim slate
    case P_IDLE:      return 0x0640;            // soft green
    case P_BUSY:      return 0xFC80;            // amber
    case P_ATTENTION: {
      // Red pulse — alternate between bright and deep red every 200ms.
      return ((now / 200) & 1) ? 0xF800 : 0x9000;
    }
  }
  return TFT_BLACK;
}

static M5Canvas frame(&M5.Display);
static bool frameReady = false;

static void uiInit() {
  M5.Display.setRotation(0);           // portrait — button at bottom
  M5.Display.setBrightness(120);
  M5.Display.fillScreen(TFT_BLACK);
  frame.setPsram(true);
  frame.setColorDepth(16);
  frame.createSprite(SW, SH);
  frameReady = frame.getBuffer() != nullptr;
}

// Dedicated pairing screen — shown when the BLE stack has an active
// 6-digit passkey. Uses font 4 because font 7 (nice big 7-segment digits)
// is too wide: 6 digits × 27 px = 162 px, won't fit 128-wide screen.
static void drawPairing(uint32_t pk) {
  if (!frameReady) return;
  frame.fillSprite(TFT_BLACK);

  // Title
  frame.setTextColor(0xFFE0);           // amber
  frame.setTextDatum(top_center);
  frame.setTextFont(2);
  frame.drawString("Pairing", SW/2, 4);

  // 6-digit passkey, big, centre
  char buf[8]; snprintf(buf, sizeof(buf), "%06lu", (unsigned long)pk);
  frame.setTextColor(TFT_CYAN);
  frame.setTextDatum(middle_center);
  frame.setTextFont(4);                 // ~14 px/char → 84 px, fits 128
  frame.drawString(buf, SW/2, SH/2);

  // Short hint
  frame.setTextColor(0x9CD3);
  frame.setTextFont(1);
  frame.setTextDatum(bottom_center);
  frame.drawString("type on desktop", SW/2, SH - 4);

  frame.pushSprite(0, 0);
}

static void drawIdle(Persona p, uint32_t now) {
  if (!frameReady) return;
  uint16_t bg = personaColor(p, now);
  frame.fillSprite(bg);

  // Tiny status label at top
  frame.setTextColor(TFT_WHITE);
  frame.setTextDatum(top_left);
  frame.setTextFont(2);
  frame.setTextSize(1);
  const char* label =
      p == P_SLEEP ? "offline" :
      p == P_BUSY  ? "working" :
      p == P_IDLE  ? "ready"   : "!";
  frame.drawString(label, 4, 4);

  // BLE state dot (top-right)
  uint16_t sigColor =
      !bleConnected() ? TFT_ORANGE :
      !bleSecure()    ? TFT_YELLOW : TFT_GREEN;
  frame.fillCircle(SW - 8, 10, 4, sigColor);

  // Centre: single useful number
  frame.setTextDatum(middle_center);
  frame.setTextFont(4);
  char buf[16];
  switch (p) {
    case P_SLEEP:
      snprintf(buf, sizeof(buf), "zZ");
      break;
    case P_BUSY:
      snprintf(buf, sizeof(buf), "R:%u", snap.running);
      break;
    case P_ATTENTION:
      snprintf(buf, sizeof(buf), "!");
      break;
    case P_IDLE:
    default:
      if (snap.tokensToday >= 1000)
        snprintf(buf, sizeof(buf), "%luK", (unsigned long)(snap.tokensToday / 1000));
      else
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)snap.tokensToday);
      break;
  }
  frame.drawString(buf, SW/2, SH/2);

  // Bottom: tiny secondary info (waiting count if non-zero)
  if (snap.waiting > 0) {
    frame.setTextFont(2);
    frame.setTextDatum(bottom_center);
    frame.setTextColor(TFT_YELLOW);
    char w[16]; snprintf(w, sizeof(w), "W:%u", snap.waiting);
    frame.drawString(w, SW/2, SH - 4);
  }

  frame.pushSprite(0, 0);
}

// Prompt screen. `holdPct` 0..100; >0 means the user is pressing-to-deny.
static void drawPrompt(uint32_t now, int holdPct) {
  if (!frameReady) return;
  uint16_t bg = ((now / 200) & 1) ? 0xF800 : 0x9000;
  frame.fillSprite(bg);

  // Tool name — big
  frame.setTextColor(TFT_WHITE);
  frame.setTextDatum(top_center);
  frame.setTextFont(4);
  char toolBuf[24];
  asciiSanitize(snap.promptTool[0] ? snap.promptTool : "?",
                toolBuf, sizeof(toolBuf));
  // Truncate to fit 128 wide (~8 chars at font 4)
  if (strlen(toolBuf) > 8) toolBuf[8] = 0;
  frame.drawString(toolBuf, SW/2, 4);

  // Hint — truncate, one line
  frame.setTextFont(2);
  frame.setTextColor(0xFFFFu);
  char hint[20];
  asciiSanitize(snap.promptHint, hint, sizeof(hint));
  if (strlen(hint) > 16) hint[16] = 0;
  frame.drawString(hint, SW/2, 38);

  // Instructions (only when not holding — swap to big DENY progress otherwise)
  if (holdPct == 0) {
    frame.setTextFont(2);
    frame.setTextColor(TFT_WHITE);
    frame.setTextDatum(middle_center);
    frame.drawString("Tap: Allow", SW/2, 72);
    frame.drawString("Hold 2s: Deny", SW/2, 92);
  } else {
    // Big DENY label + fill bar
    frame.setTextFont(4);
    frame.setTextColor(TFT_WHITE);
    frame.setTextDatum(middle_center);
    frame.drawString("DENY", SW/2, 72);

    int barW = SW - 20;
    int filled = (barW * holdPct) / 100;
    frame.drawRect(10, 96, barW, 14, TFT_WHITE);
    frame.fillRect(11, 97, filled, 12, TFT_WHITE);
  }

  frame.pushSprite(0, 0);
}

// ---------- input: single-button short/long press with cancellable hold -----
static uint32_t pressStartMs = 0;
static bool     pressActive  = false;
static bool     denySent     = false;

// Returns true if a decision was made this call.
static bool handleButtonForPrompt() {
  if (snap.promptId[0] == 0) { pressActive = false; denySent = false; return false; }
  if (strcmp(snap.promptId, lastRepliedId) == 0) return false;

  if (M5.BtnA.wasPressed()) {
    pressStartMs = millis();
    pressActive = true;
    denySent = false;
  }

  if (pressActive && !denySent && M5.BtnA.isPressed()) {
    uint32_t held = millis() - pressStartMs;
    // Audible tick at 1s as a halfway cue
    static bool tickedHalf = false;
    if (held >= 1000 && !tickedHalf) { soundHoldTick(); tickedHalf = true; }
    if (held >= 2000) {
      sendPermission(snap.promptId, "deny");
      soundDeny();
      denySent = true;
      tickedHalf = false;
      return true;
    }
  }

  if (M5.BtnA.wasReleased() && pressActive) {
    uint32_t held = millis() - pressStartMs;
    pressActive = false;
    if (!denySent && held < 500) {
      // Short click → Allow
      sendPermission(snap.promptId, "once");
      soundAllow();
      return true;
    }
    // else: mid-hold release — treat as cancel, nothing to do
  }
  return false;
}

static int holdProgressPct() {
  if (!pressActive || denySent || snap.promptId[0] == 0) return 0;
  uint32_t held = millis() - pressStartMs;
  int pct = (int)((held * 100) / 2000);
  if (pct > 100) pct = 100;
  return pct;
}

// ---------- debug serial ----------------------------------------------------
static void pumpSerialDebug() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 'p':
        snprintf(snap.promptId, sizeof(snap.promptId), "debug_%lu",
                 (unsigned long)millis());
        strcpy(snap.promptTool, "Bash");
        strcpy(snap.promptHint, "rm -rf /tmp");
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
    }
  }
}

// ---------- setup / loop ----------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true;
  // Tell M5Unified an Atom Echo Base is stacked underneath so M5.Speaker
  // routes through its I2S DAC+amp.
  cfg.external_speaker.atomic_echo = true;
  M5.begin(cfg);

  uiInit();

  M5.Speaker.setVolume(180);

  Serial.begin(115200);
  delay(50);
  Serial.println("\n[buddy] boot (AtomS3R+Echo)");

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char name[32];
  snprintf(name, sizeof(name), "Claude AtomS3R %02X%02X", mac[4], mac[5]);
  bleInit(name);
}

void loop() {
  M5.update();
  pumpBleLines();
  pumpSerialDebug();

  // Detect freshly-arrived prompt → chime + reset touch state
  bool hasPrompt = snap.promptId[0] != 0;
  bool newPrompt = hasPrompt && strcmp(snap.promptId, lastPromptId) != 0;
  if (newPrompt) {
    strncpy(lastPromptId, snap.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    lastRepliedId[0] = 0;
    pressActive = false;
    denySent = false;
    soundPrompt();
  }
  if (!hasPrompt) { lastPromptId[0] = 0; lastRepliedId[0] = 0; }

  // Handle button for prompt decision
  if (hasPrompt) {
    if (handleButtonForPrompt()) {
      // For local debug prompts the desktop won't retract; clear ourselves.
      if (strncmp(snap.promptId, "debug_", 6) == 0) {
        snap.promptId[0] = 0; snap.promptTool[0] = 0;
        snap.promptHint[0] = 0; snap.waiting = 0;
      }
    }
  }

  // Render ~20 Hz so the red pulse and hold bar feel smooth.
  // Priority: pairing passkey > prompt > idle.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 50) {
    lastDraw = millis();
    uint32_t pk = blePasskey();
    if (pk)             drawPairing(pk);
    else if (hasPrompt) drawPrompt(millis(), holdProgressPct());
    else                drawIdle(derivePersona(), millis());
  }

  delay(5);
}
