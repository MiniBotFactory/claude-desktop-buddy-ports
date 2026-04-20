// ASCII-art cat buddy, ported from upstream src/buddies/cat.cpp but
// redrawn on M5GFX (M5Canvas) instead of TFT_eSPI sprites.
//
// 4 persona states supported (sleep / idle / busy / attention). Upstream
// has celebrate/dizzy/heart too — add them later if you want.
//
// Rendering uses the built-in 5x7 GFX font at textSize=2 → 12x16 px per
// character. Each pose is 12 chars wide x 5 rows = 144x80 canvas.

#include "cat_buddy.h"

static constexpr uint16_t CAT_BODY  = 0xC2A6;  // warm orange
static constexpr uint16_t CAT_DIM   = 0x7BEF;
static constexpr uint16_t CAT_YEL   = 0xFFE0;
static constexpr uint16_t CAT_WHITE = 0xFFFF;

// Print a 5-row 12-col ASCII sprite centered in the canvas, with optional
// x/y pixel offsets for jitter / bob effects.
static void drawPose(M5Canvas* c, const char* const* lines, uint16_t color,
                     int xOff = 0, int yOff = 0) {
  c->setTextColor(color);
  c->setTextSize(2);
  c->setTextFont(1);  // built-in GFX font
  const int charW = 12, charH = 16;
  const int rows = 5;
  for (int r = 0; r < rows; r++) {
    int x = xOff;
    int y = yOff + r * charH;
    c->setCursor(x, y);
    c->print(lines[r]);
  }
}

static void particle(M5Canvas* c, int x, int y, const char* s, uint16_t color) {
  c->setTextColor(color);
  c->setTextSize(2);
  c->setTextFont(1);
  c->setCursor(x, y);
  c->print(s);
}

// Spring flower crown. Per-persona style table below — the crown's mood
// tracks the cat's mood, so at a glance (even without reading the screen)
// you know what Claude is up to:
//   SLEEP     — wilted grey, static         (nothing's happening)
//   IDLE      — cherry pink, gentle bob     (calm spring)
//   BUSY      — orange + emerald, fast bob  (energetic)
//   ATTENTION — vivid red, high-freq flicker (something needs you)
struct CrownStyle {
  uint16_t flower;    // flower color
  uint16_t leaf;      // leaf color
  uint16_t flowerAlt; // alternate flower hue (for subtle shimmer)
  uint16_t leafAlt;
  uint16_t bobMs;     // vertical wobble period; 0 = static
  uint8_t  bobPx;     // vertical amplitude in px
  uint16_t hueMs;     // color-swap period
  bool     flicker;   // random size flicker for urgency
};

static const CrownStyle CROWN_STYLES[4] = {
  // SLEEP — drooping, muted
  { 0x39E7, 0x2A46, 0x39E7, 0x2A46,    0, 0,    0, false },
  // IDLE — cherry pink + tender green, calm
  { 0xFBD5, 0x6720, 0xFB2C, 0x7760,  800, 1, 1100, false },
  // BUSY — bright orange + emerald, livelier
  { 0xFD00, 0x0680, 0xFBC0, 0x2E80,  300, 2,  350, false },
  // ATTENTION — vivid red + dark green, urgent flicker
  { 0xF800, 0x03E0, 0xFA00, 0x0420,  150, 2,  200, true },
};

static void drawCrown(M5Canvas* c, uint32_t t, Persona p) {
  const CrownStyle& s = CROWN_STYLES[p];

  static const char GLYPH[5]     = { '*', 'v', '*', 'v', '*' };
  static const bool IS_FLOWER[5] = { true, false, true, false, true };

  c->setTextSize(2);
  c->setTextFont(1);

  int bob = 0;
  if (s.bobMs) bob = ((t / s.bobMs) & 1) ? 0 : -(int)s.bobPx;

  bool alt = s.hueMs ? ((t / s.hueMs) & 1) : false;

  // Urgency flicker — every ~80ms, 30% chance a petal drops out, and the
  // text size briefly jumps to 3. Only on ATTENTION.
  bool bigPetal = false;
  if (s.flicker) bigPetal = ((t / 90) % 3 == 0);

  for (int i = 0; i < 5; i++) {
    bool localAlt = alt ^ (i & 1);
    uint16_t color = IS_FLOWER[i]
        ? (localAlt ? s.flowerAlt : s.flower)
        : (localAlt ? s.leafAlt   : s.leaf);

    c->setTextColor(color);
    // Flicker: pump up flower size on some frames for urgency
    if (s.flicker && IS_FLOWER[i] && bigPetal) {
      c->setTextSize(3);
    } else {
      c->setTextSize(2);
    }
    int y = bob + (IS_FLOWER[i] ? 0 : 2);
    c->setCursor(24 + i * 18, y);
    char str[2] = { GLYPH[i], 0 };
    c->print(str);
  }
  c->setTextSize(2);   // restore for anything drawn after
}

// --- sleep ---
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const LOAF[5]     = { "            ", "            ", "   .-..-.   ", "  ( -.- )   ", "  `------`~ " };
  static const char* const CURL[5]     = { "            ", "            ", "   .-/\\.    ", "  (  ..  )) ", "  `~~~~~~`  " };
  static const char* const PURR[5]     = { "            ", "            ", "   .-..-.   ", "  ( u.u )   ", " `~------'~ " };
  const char* const* seq[] = { LOAF, LOAF, PURR, CURL, PURR, LOAF };
  uint8_t beat = (t / 800) % 6;
  drawPose(c, seq[beat], CAT_BODY);
  // Wilted crown drapes on the sleeping cat
  drawCrown(c, t, P_SLEEP);
  // Zzz trails
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}

// --- idle ---
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const REST[5]    = { "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const LOOK_L[5]  = { "            ", "   /\\_/\\    ", "  (o    o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const LOOK_R[5]  = { "            ", "   /\\_/\\    ", "  ( o    o) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const BLINK[5]   = { "            ", "   /\\_/\\    ", "  ( -   - ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const TAIL[5]    = { "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")~  " };
  const char* const* seq[] = { REST, REST, BLINK, REST, LOOK_L, REST, LOOK_R, REST, TAIL, REST };
  uint8_t beat = (t / 700) % 10;
  drawPose(c, seq[beat], CAT_BODY);
  drawCrown(c, t, P_IDLE);
}

// --- busy ---
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const PAW_UP[5]  = { "      .     ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )/ ", "  (\")_(\")   " };
  static const char* const PAW_TAP[5] = { "    .       ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )_ ", "  (\")_(\")   " };
  static const char* const STARE[5]   = { "            ", "   /\\_/\\    ", "  ( O   O ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const SMUG[5]    = { "            ", "   /\\_/\\    ", "  ( -   - ) ", "  (  w   )  ", "  (\")_(\")   " };
  const char* const* seq[] = { PAW_UP, PAW_TAP, PAW_UP, PAW_TAP, STARE, SMUG };
  uint8_t beat = (t / 400) % 6;
  drawPose(c, seq[beat], CAT_BODY);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  particle(c, 110, 28, DOTS[(t / 150) % 6], CAT_WHITE);
  drawCrown(c, t, P_BUSY);
}

// --- attention ---
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALERT[5]   = { "            ", "   /^_^\\    ", "  ( O   O ) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const SCAN_L[5]  = { "            ", "   /^_^\\    ", "  (O    O ) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const SCAN_R[5]  = { "            ", "   /^_^\\    ", "  ( O    O) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const HISS[5]    = { "            ", "   /^_^\\    ", "  ( O   O ) ", "  (  >   )  ", "  (\")_(\")   " };
  const char* const* seq[] = { ALERT, SCAN_L, ALERT, SCAN_R, HISS, ALERT };
  uint8_t beat = (t / 300) % 6;
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[beat], CAT_BODY, jitter, 0);

  // Blinking ! marks
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}

// ============================================================================
// Owl buddy — poses ported from upstream/src/buddies/owl.cpp. Same crown +
// drawPose/particle helpers as the cat so behaviour matches.
// ============================================================================

namespace owl {
static constexpr uint16_t OWL_BODY = 0x8430;  // muted brown-grey

static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const TUCK[5]   = { "            ", "   .-..-.   ", "  ( -  - )  ", "  (  __  )  ", "   `----'   " };
  static const char* const PUFF[5]   = { "    .--.    ", "   /-..-\\   ", "  ( -  - )  ", "  (  __  )  ", "   `----'   " };
  static const char* const DEEP[5]   = { "    .--.    ", "   /-..-\\   ", "  ( _  _ )  ", "  (  ZZ  )  ", "   `----'   " };
  static const char* const HEAD_L[5] = { "            ", "   .-..-.   ", " ( -  -  )  ", "  (  __  )  ", "   `----'   " };
  static const char* const HEAD_R[5] = { "            ", "   .-..-.   ", "  (  - - -) ", "  (  __  )  ", "   `----'   " };
  const char* const* seq[] = { TUCK, PUFF, DEEP, HEAD_L, DEEP, HEAD_R };
  uint8_t beat = (t / 800) % 6;
  drawPose(c, seq[beat], OWL_BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p / 2, 8 - p / 2, "Z", CAT_WHITE);
}

static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const REST[5]   = { "            ", "   /\\  /\\   ", "  ((O)(O))  ", "  (  ><  )  ", "   `----'   " };
  static const char* const BLINK[5]  = { "            ", "   /\\  /\\   ", "  ((-)(-))  ", "  (  ><  )  ", "   `----'   " };
  static const char* const LOOK_L[5] = { "            ", "   /\\  /\\   ", "  ((O)(O))  ", " (  ><   )  ", "   `----'   " };
  static const char* const LOOK_R[5] = { "            ", "   /\\  /\\   ", "  ((O)(O))  ", "  (  ><  ) ", "   `----'   " };
  static const char* const WINK[5]   = { "            ", "   /\\  /\\   ", "  ((O)(-))  ", "  (  ><  )  ", "   `----'   " };
  const char* const* seq[] = { REST, REST, BLINK, REST, LOOK_L, REST, LOOK_R, REST, WINK, REST };
  uint8_t beat = (t / 700) % 10;
  drawPose(c, seq[beat], OWL_BODY);
  drawCrown(c, t, P_IDLE);
}

static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const SCROLL[5] = { "    [___]   ", "   /\\  /\\   ", "  ((v)(v))  ", "  (  --  )  ", "   `----'   " };
  static const char* const PECK_A[5] = { "    [___]   ", "   /\\  /\\   ", "  ((v)(v))  ", "  (  >>  )  ", "   `----'   " };
  static const char* const PECK_B[5] = { "    [___]   ", "   /\\  /\\   ", "  ((v)(v))  ", "  (  <<  )  ", "   `----'   " };
  static const char* const PONDER[5] = { "      ?     ", "   /\\  /\\   ", "  ((^)(^))  ", "  (  ..  )  ", "   `----'   " };
  const char* const* seq[] = { SCROLL, PECK_A, PECK_B, PECK_A, PONDER, SCROLL };
  uint8_t beat = (t / 400) % 6;
  drawPose(c, seq[beat], OWL_BODY);
  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  particle(c, 110, 28, DOTS[(t / 150) % 6], CAT_WHITE);
  drawCrown(c, t, P_BUSY);
}

static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALERT[5]  = { "            ", "  /^\\  /^\\  ", " ((O))((O)) ", " (   ><   ) ", "  `------'  " };
  static const char* const SCAN_L[5] = { "            ", "  /^\\  /^\\  ", "((O))((O))  ", " (   ><   ) ", "  `------'  " };
  static const char* const SCAN_R[5] = { "            ", "  /^\\  /^\\  ", "  ((O))((O))", " (   ><   ) ", "  `------'  " };
  static const char* const GLARE[5]  = { "            ", "  /^\\  /^\\  ", " ((-))((-)) ", " (   ><   ) ", "  `------'  " };
  const char* const* seq[] = { ALERT, SCAN_L, ALERT, SCAN_R, GLARE, ALERT };
  uint8_t beat = (t / 300) % 6;
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[beat], OWL_BODY, jitter, 0);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace owl

// ============================================================================
// Dispatch
// ============================================================================

static BuddyKind g_kind = BUDDY_CAT;

void buddySetKind(BuddyKind k) {
  if (k >= BUDDY_COUNT) k = BUDDY_CAT;
  g_kind = k;
}
BuddyKind buddyGetKind(void) { return g_kind; }

const char *buddyKindName(BuddyKind k) {
  switch (k) {
    case BUDDY_CAT: return "cat";
    case BUDDY_OWL: return "owl";
    default:        return "?";
  }
}

void catDrawFrame(M5Canvas* canvas, Persona p, uint32_t tickMs) {
  if (g_kind == BUDDY_OWL) {
    switch (p) {
      case P_SLEEP:     owl::doSleep(canvas, tickMs); break;
      case P_IDLE:      owl::doIdle(canvas, tickMs); break;
      case P_BUSY:      owl::doBusy(canvas, tickMs); break;
      case P_ATTENTION: owl::doAttention(canvas, tickMs); break;
    }
    return;
  }
  // Default: cat
  switch (p) {
    case P_SLEEP:     doSleep(canvas, tickMs); break;
    case P_IDLE:      doIdle(canvas, tickMs); break;
    case P_BUSY:      doBusy(canvas, tickMs); break;
    case P_ATTENTION: doAttention(canvas, tickMs); break;
  }
}
