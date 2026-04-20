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
// Vertical layout inside the 150x108 pet canvas:
//   y=[0..CROWN_Y_BASE..)   empty gap above crown
//   y=[CROWN_Y_BASE..+10]   flower crown (~10 px tall)
//   y=[POSE_Y_BASE..+80]    5-row ASCII pose (textSize 2 → 16 px/row)
//   y=[POSE_Y_BASE+80..108] small slack below pose
//
// Pose pushed all the way to canvas bottom so the pet's feet sit on the
// divider rather than floating halfway up. Crown is placed just above the
// pose head so ear/hat pixels in pose row 0 never punch through the crown.
// Particles (Z's, "!" marks, "..." dots) are co-located with the pose, so
// particle() auto-applies POSE_Y_BASE — species code can keep its original
// upstream y values that assumed a top-aligned pose.
static constexpr int POSE_Y_BASE  = 28;   // pose row 0 y
static constexpr int CROWN_Y_BASE = 14;   // crown glyph row y (just above pose)

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
    int y = POSE_Y_BASE + yOff + r * charH;
    c->setCursor(x, y);
    c->print(lines[r]);
  }
}

static void particle(M5Canvas* c, int x, int y, const char* s, uint16_t color) {
  c->setTextColor(color);
  c->setTextSize(2);
  c->setTextFont(1);
  // particles follow the pose so their relative y (as written in each
  // species, often taken from upstream) stays correct.
  c->setCursor(x, POSE_Y_BASE + y);
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

// Each species' head occupies a different column range in its 12-char
// pose string, so a single crown layout doesn't sit correctly on every
// pet. Baked table: x offset applied on top of the base "center at col 5"
// crown so the flowers line up with the pet's actual head midpoint.
//
// Example measurements (col = 0-indexed, textSize 2 → 12 px per col):
//   cat "   /\_/\    "    head cols 3..7   → col 5   → offset 0
//   owl "   /\  /\   "    head cols 3..10  → col 6.5 → offset +18
//   duck "    __     "    head cols 4..5   → col 4.5 → offset -6
//   penguin ".---."       head cols 3..7   → col 5   → offset 0
//   rabbit "    (\_/)"    head cols 4..8   → col 6   → offset +12
//   dragon "/^\  /^\"     head cols 2..9   → col 5.5 → offset +6
//   ghost ".----."        head cols 3..8   → col 5.5 → offset +6
//   robot "[____]"        head cols 3..8   → col 5.5 → offset +6
static int crownXOffset(void) {
  switch (buddyGetKind()) {
    case BUDDY_OWL:     return 6;     // was 18 — owl head cols are 3-8 (centre 5.5), not 3-10
    case BUDDY_DUCK:    return -6;
    case BUDDY_RABBIT:  return 12;
    case BUDDY_DRAGON:  return 6;
    case BUDDY_GHOST:   return 6;
    case BUDDY_ROBOT:   return 6;
    case BUDDY_CAT:
    case BUDDY_PENGUIN:
    default:            return 0;
  }
}

static void drawCrown(M5Canvas* c, uint32_t t, Persona p) {
  // ATTENTION is the "someone needs to approve right now" state. The pet's
  // own urgent animation (alert eyes, jitter, '!' marks) is the signal —
  // layering the rainbow crown on top makes the screen busy and pulls the
  // eye away from the pet's body language. Keep the crown for the calm
  // personas only.
  if (p == P_ATTENTION) return;
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
    int y = CROWN_Y_BASE + bob + (IS_FLOWER[i] ? 0 : 2);
    c->setCursor(24 + crownXOffset() + i * 18, y);
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
// Duck buddy — adapted from upstream src/buddies/duck.cpp. Yellow.
// ============================================================================
namespace duck {
static constexpr uint16_t BODY = 0xFFE0;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const TUCK[5]    = { "            ", "            ", "    __      ", "  <(-_)_)   ", " ~~~~~~~~~~ " };
  static const char* const BREATHE[5] = { "            ", "    __      ", "  <(-_)_)   ", "  ~~~~~~~~  ", "   ~~~~~~   " };
  static const char* const DREAM[5]   = { "            ", "    __      ", "  <(uu)_)   ", " ~~~~~~~~~~ ", "  ~~~~~~~~  " };
  const char* const* seq[] = { TUCK, BREATHE, DREAM, BREATHE, TUCK, BREATHE };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const REST[5]    = { "            ", "    __      ", "  <(o )___  ", "   (  ._>   ", "    `--'    " };
  static const char* const LOOK_L[5]  = { "            ", "    __      ", " <<(o )___  ", "   (  ._>   ", "    `--'    " };
  static const char* const LOOK_R[5]  = { "            ", "    __      ", "  <( o)___  ", "   (  ._>   ", "    `--'    " };
  static const char* const BLINK[5]   = { "            ", "    __      ", "  <(- )___  ", "   (  ._>   ", "    `--'    " };
  const char* const* seq[] = { REST, REST, BLINK, REST, LOOK_L, REST, LOOK_R, REST, BLINK, REST };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const PADDLE_A[5] = { "            ", "    __      ", "  <(o )___  ", "   (  ._>   ", "  ~ `--'    " };
  static const char* const PADDLE_B[5] = { "            ", "    __      ", "  <(o )___  ", "   (  ._>   ", "    `--' ~  " };
  static const char* const THINK[5]    = { "      ?     ", "    __      ", "  <(o )___  ", "   (  ._>   ", "    `--'    " };
  const char* const* seq[] = { PADDLE_A, PADDLE_B, PADDLE_A, PADDLE_B, THINK, PADDLE_A };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALERT[5]   = { "    __      ", "  <(O )     ", "  (    )___ ", "   (  ._>   ", "    `--'    " };
  static const char* const SCAN_L[5]  = { "    __      ", " <<(O )     ", "  (    )___ ", "   (  ._>   ", "    `--'    " };
  static const char* const SCAN_R[5]  = { "    __      ", "  <( O)     ", "  (    )___ ", "   (  ._>   ", "    `--'    " };
  static const char* const HONK[5]    = { "    __      ", "  <O(O )    ", "  (    )___ ", "   (  ._>   ", "    `--'    " };
  const char* const* seq[] = { ALERT, SCAN_L, ALERT, SCAN_R, HONK, ALERT };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace duck

// ============================================================================
// Penguin buddy — cyan, formal
// ============================================================================
namespace penguin {
static constexpr uint16_t BODY = 0x041F;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const TUCK[5]   = { "            ", "   .---.    ", "  ( -- )    ", "  (_____)   ", "   ~~~~~    " };
  static const char* const SNORE[5]  = { "    o O .   ", "   .---.    ", "  ( __ )    ", "  (_____)   ", "   =====    " };
  static const char* const TIPPED[5] = { "            ", "            ", "  .-----.   ", " ( zz   )=> ", "  `~~~~~`   " };
  const char* const* seq[] = { TUCK, TUCK, SNORE, TIPPED, SNORE, TUCK };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", 0x07FFu);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const STAND[5]  = { "   .---.    ", "  ( o>o )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const WAD_L[5]  = { "   .---.    ", "  ( o>o )   ", "/(     )    ", "  `-----`   ", "  J    L    " };
  static const char* const WAD_R[5]  = { "   .---.    ", "  ( o>o )   ", " (     )\\   ", "  `-----`   ", "   J    L   " };
  static const char* const BLINK[5]  = { "   .---.    ", "  ( ->- )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  const char* const* seq[] = { STAND, WAD_L, STAND, WAD_R, STAND, BLINK, STAND, WAD_L, STAND, WAD_R };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const TYPE_A[5] = { "   .---.    ", "  ( v>v )   ", " /(     )\\  ", " /`-----`\\  ", "   J   L    " };
  static const char* const TYPE_B[5] = { "   .---.    ", "  ( v>v )   ", " \\(     )/  ", " \\`-----`/  ", "   J   L    " };
  static const char* const THINK[5]  = { "      ?     ", "   .---.    ", "  ( ^>^ )   ", " /(  .  )\\  ", "   J   L    " };
  const char* const* seq[] = { TYPE_A, TYPE_B, TYPE_A, TYPE_B, THINK, TYPE_A };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALERT[5]  = { "   .---.    ", "  ( O>O )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SCAN_L[5] = { "   .---.    ", "  (O> O )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SCAN_R[5] = { "   .---.    ", "  ( O >O)   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const TENSE[5]  = { "  /.---.\\   ", " /( O>O )\\  ", "//(     )\\\\ ", "  `-----`   ", "  J     L   " };
  const char* const* seq[] = { ALERT, SCAN_L, ALERT, SCAN_R, TENSE, ALERT };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace penguin

// ============================================================================
// Rabbit buddy — white with long ears
// ============================================================================
namespace rabbit {
static constexpr uint16_t BODY = 0xFFFFu;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const CURL[5]    = { "            ", "    (\\_/)   ", "   ( -.- )  ", "  (zzz___)  ", "   `\"\"\"\"`   " };
  static const char* const BREATHE[5] = { "            ", "    (\\_/)   ", "   ( -_- )  ", "  (___zz_)  ", "   `\"\"\"\"`   " };
  static const char* const DREAM[5]   = { "            ", "    (\\_/)   ", "   ( u.u )  ", "  (___oo_)  ", "   `\"\"\"\"`   " };
  const char* const* seq[] = { CURL, BREATHE, CURL, BREATHE, DREAM, BREATHE };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const REST[5]   = { "    (\\_/)   ", "   ( o o )  ", "  =(  v  )= ", "   (\")_(\")  ", "            " };
  static const char* const LOOK_L[5] = { "    (\\_/)   ", "   (o  o )  ", "  =(  v  )= ", "   (\")_(\")  ", "            " };
  static const char* const LOOK_R[5] = { "    (\\_/)   ", "   ( o  o)  ", "  =(  v  )= ", "   (\")_(\")  ", "            " };
  static const char* const BLINK[5]  = { "    (\\_/)   ", "   ( - - )  ", "  =(  v  )= ", "   (\")_(\")  ", "            " };
  const char* const* seq[] = { REST, REST, BLINK, REST, LOOK_L, REST, LOOK_R, REST, BLINK, REST };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const DIG_A[5]  = { "    (\\_/)   ", "   ( v v )  ", "  =(  v  )= ", "  /(\")_(\")\\ ", "  ~~~~~~~~  " };
  static const char* const DIG_B[5]  = { "    (\\_/)   ", "   ( v v )  ", "  =(  v  )= ", "  \\(\")_(\")/  ", "  ~~~~~~~~  " };
  static const char* const THINK[5]  = { "  ? (\\_/)   ", "   ( o o )  ", "  =(  v  )= ", "   (\")_(\")  ", "            " };
  const char* const* seq[] = { DIG_A, DIG_B, DIG_A, DIG_B, THINK, DIG_A };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALERT[5]  = { "   /|  |\\   ", "  /(\\_/)\\   ", "  ( O  O )  ", "  =(  v  )= ", "   (\")_(\")  " };
  static const char* const SCAN_L[5] = { "   /|  |\\   ", "  /(\\_/)\\   ", "  (O   O )  ", "  =(  v  )= ", "   (\")_(\")  " };
  static const char* const SCAN_R[5] = { "   /|  |\\   ", "  /(\\_/)\\   ", "  ( O   O)  ", "  =(  v  )= ", "   (\")_(\")  " };
  static const char* const HUSH[5]   = { "   /|  |\\   ", "   (\\_/)    ", "  ( o  o )  ", "  =(  .  )= ", "   (\")_(\")  " };
  const char* const* seq[] = { ALERT, SCAN_L, ALERT, SCAN_R, HUSH, ALERT };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace rabbit

// ============================================================================
// Dragon buddy — red, mythical
// ============================================================================
namespace dragon {
static constexpr uint16_t BODY = 0xF800u;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const CURL[5]   = { "            ", "            ", "   _____    ", "  (--   )~  ", "  `vvvvv'   " };
  static const char* const BREATH[5] = { "            ", "            ", "   _____    ", "  (--   )~~ ", "  `vvvvv'   " };
  static const char* const PUFF[5]   = { "            ", "       o    ", "   _____    ", "  (--   )~~ ", "  `vvvvv'   " };
  const char* const* seq[] = { CURL, BREATH, CURL, PUFF, BREATH, CURL };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const PROUD[5]  = { "            ", "  /^\\  /^\\  ", " <  o    o >", " (   ww   ) ", "  `-vvvv-'  " };
  static const char* const BLINK[5]  = { "            ", "  /^\\  /^\\  ", " <  -    - >", " (   ww   ) ", "  `-vvvv-'  " };
  static const char* const WING_U[5] = { "  /^\\  /^\\  ", "  \\_/  \\_/  ", " <  o    o >", " (   ww   ) ", "  `-vvvv-'  " };
  static const char* const WING_D[5] = { "            ", "  \\v/  \\v/  ", " <  o    o >", " (   ww   ) ", "  `-vvvv-'  " };
  const char* const* seq[] = { PROUD, PROUD, WING_U, WING_D, PROUD, BLINK, PROUD, WING_U, WING_D, PROUD };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const COUNT_A[5] = { "    $$$$    ", "  /^\\  /^\\  ", " <  v    v >", " (   --   ) ", " /`-vvvv-'\\ " };
  static const char* const COUNT_B[5] = { "    $$$$    ", "  /^\\  /^\\  ", " <  v    v >", " (   __   ) ", " \\`-vvvv-'/ " };
  static const char* const PONDER[5]  = { "      ?     ", "  /^\\  /^\\  ", " <  ^    ^ >", " (   ..   ) ", "  `-vvvv-'  " };
  const char* const* seq[] = { COUNT_A, COUNT_B, COUNT_A, COUNT_B, PONDER, COUNT_A };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ROAR[5]   = { "    ^  ^    ", " /^^\\  /^^\\ ", "<  O    O  >", " (   <>   ) ", "  `-vvvv-'  " };
  static const char* const FLAME[5]  = { "  ~~~  ~~~  ", " /^^\\  /^^\\ ", "<  O    O  >", " (   <>   )~", "  `-vvvv-'  " };
  static const char* const SCAN_L[5] = { "    ^  ^    ", " /^^\\  /^^\\ ", "< O      O >", " (   O    ) ", "  `-vvvv-'  " };
  static const char* const HISS[5]   = { "    ^  ^    ", " /^^\\  /^^\\ ", "<  o    o  >", " (   ss   ) ", "  `-vvvv-'  " };
  const char* const* seq[] = { ROAR, FLAME, ROAR, SCAN_L, HISS, ROAR };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace dragon

// ============================================================================
// Ghost buddy — white/translucent
// ============================================================================
namespace ghost {
static constexpr uint16_t BODY = 0xFFFFu;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const DRIFT[5]   = { "            ", "   .----.   ", "  ( -    - )", "  |        |", "  ~`~``~`~  " };
  static const char* const FADE[5]    = { "            ", "   . -- .   ", "  ( -    - )", "  .        .", "  . . . . . " };
  const char* const* seq[] = { DRIFT, DRIFT, FADE, DRIFT, FADE, DRIFT };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const HOVER[5]  = { "            ", "   .----.   ", "  ( O    O )", "  |   u    |", "  `~`~~`~`  " };
  static const char* const LOOK_L[5] = { "            ", "   .----.   ", "  (O      O)", "  |   u    |", "  `~`~~`~`  " };
  static const char* const LOOK_R[5] = { "            ", "   .----.   ", "  ( O    O )", "  |    u   |", "  `~`~~`~`  " };
  static const char* const BLINK[5]  = { "            ", "   .----.   ", "  ( -    - )", "  |   u    |", "  `~`~~`~`  " };
  const char* const* seq[] = { HOVER, HOVER, BLINK, HOVER, LOOK_L, HOVER, LOOK_R, HOVER, BLINK, HOVER };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const SPIN_L[5] = { "            ", "   .----.   ", "  (<    >)  ", "  |   w    |", "  `~`~~`~`  " };
  static const char* const SPIN_R[5] = { "            ", "   .----.   ", "  (>    <)  ", "  |   w    |", "  `~`~~`~`  " };
  static const char* const THINK[5]  = { "     ?      ", "   .----.   ", "  ( o    o )", "  |   .    |", "  `~`~~`~`  " };
  const char* const* seq[] = { SPIN_L, SPIN_R, SPIN_L, SPIN_R, THINK, SPIN_L };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const BOO[5]    = { "            ", "  /.----.\\  ", " /(O    O)\\ ", "|  BOO!    |", " `~`~~`~`~  " };
  static const char* const SPOOK_L[5]= { "            ", " /.----.    ", " (O     O)  ", " |  O      |", " `~`~~`~`   " };
  static const char* const SPOOK_R[5]= { "            ", "   .----.\\  ", "  (O     O) ", "  |     O  |", "   `~`~~`~` " };
  static const char* const WAVE[5]   = { "   ~~~~~~   ", "   .----.   ", "  ( O    O )", "  |   !    |", "  ~~~~~~~~  " };
  const char* const* seq[] = { BOO, SPOOK_L, BOO, SPOOK_R, WAVE, BOO };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace ghost

// ============================================================================
// Robot buddy — silver, industrial
// ============================================================================
namespace robot {
static constexpr uint16_t BODY = 0xBDF7u;
static void doSleep(M5Canvas* c, uint32_t t) {
  static const char* const STBY[5]   = { "            ", "   [____]   ", "  [      ]  ", "  [  --  ]  ", "   '----'   " };
  static const char* const BLINK[5]  = { "            ", "   [____]   ", "  [      ]  ", "  [  ..  ]  ", "   '----'   " };
  static const char* const SAVE[5]   = { "            ", "   [____]   ", "  [ zz   ]  ", "  [  __  ]  ", "   '----'   " };
  const char* const* seq[] = { STBY, BLINK, SAVE, BLINK, STBY, STBY };
  drawPose(c, seq[(t / 800) % 6], BODY);
  int p = (t / 200) % 10;
  particle(c, 110 + p, 16 - p, "z", CAT_DIM);
  particle(c, 120 + p/2, 8 - p/2, "Z", CAT_WHITE);
}
static void doIdle(M5Canvas* c, uint32_t t) {
  static const char* const READY[5]  = { "            ", "   [____]   ", "  [ o  o ]  ", "  [  <>  ]  ", "   /----\\   " };
  static const char* const LOOK_L[5] = { "            ", "   [____]   ", "  [o   o ]  ", "  [  <>  ]  ", "   /----\\   " };
  static const char* const LOOK_R[5] = { "            ", "   [____]   ", "  [ o   o]  ", "  [  <>  ]  ", "   /----\\   " };
  static const char* const BLINK[5]  = { "            ", "   [____]   ", "  [ -  - ]  ", "  [  <>  ]  ", "   /----\\   " };
  const char* const* seq[] = { READY, READY, BLINK, READY, LOOK_L, READY, LOOK_R, READY, BLINK, READY };
  drawPose(c, seq[(t / 700) % 10], BODY);
  drawCrown(c, t, P_IDLE);
}
static void doBusy(M5Canvas* c, uint32_t t) {
  static const char* const WORK_A[5] = { "            ", "   [____]   ", "  [ * .* ]  ", "  [ ==== ]  ", "   /----\\   " };
  static const char* const WORK_B[5] = { "            ", "   [____]   ", "  [ .* * ]  ", "  [ ==== ]  ", "   /----\\   " };
  static const char* const THINK[5]  = { "    .--.    ", "   [____]   ", "  [ ?  ? ]  ", "  [  <>  ]  ", "   /----\\   " };
  const char* const* seq[] = { WORK_A, WORK_B, WORK_A, WORK_B, THINK, WORK_A };
  drawPose(c, seq[(t / 400) % 6], BODY);
  drawCrown(c, t, P_BUSY);
}
static void doAttention(M5Canvas* c, uint32_t t) {
  static const char* const ALARM[5]  = { "  [!!]  [!!]", "   [____]   ", "  [ OO OO]  ", "  [  !!  ]  ", "   /----\\   " };
  static const char* const SCAN_L[5] = { "  [!!]      ", "   [____]   ", "  [OO  OO]  ", "  [  !!  ]  ", "   /----\\   " };
  static const char* const SCAN_R[5] = { "       [!!] ", "   [____]   ", "  [OO  OO]  ", "  [  !!  ]  ", "   /----\\   " };
  static const char* const HOT[5]    = { "  [!!]  [!!]", "  /[____]\\  ", " /[ XX XX]\\ ", "  [  !!  ]  ", "   /----\\   " };
  const char* const* seq[] = { ALARM, SCAN_L, ALARM, SCAN_R, HOT, ALARM };
  int jitter = (t / 100) & 1 ? 1 : -1;
  drawPose(c, seq[(t / 300) % 6], BODY, jitter);
  if ((t / 200) & 1) particle(c, 50, -4, "!", CAT_YEL);
  if ((t / 300) & 1) particle(c, 78, -4, "!", CAT_YEL);
  drawCrown(c, t, P_ATTENTION);
}
} // namespace robot

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
    case BUDDY_CAT:     return "cat";
    case BUDDY_OWL:     return "owl";
    case BUDDY_DUCK:    return "duck";
    case BUDDY_PENGUIN: return "penguin";
    case BUDDY_RABBIT:  return "rabbit";
    case BUDDY_DRAGON:  return "dragon";
    case BUDDY_GHOST:   return "ghost";
    case BUDDY_ROBOT:   return "robot";
    default:            return "?";
  }
}

// Macro to call the right species function for each persona.
#define DISPATCH(ns) do { \
    switch (p) { \
      case P_SLEEP:     ns::doSleep(canvas, tickMs); break; \
      case P_IDLE:      ns::doIdle(canvas, tickMs); break; \
      case P_BUSY:      ns::doBusy(canvas, tickMs); break; \
      case P_ATTENTION: ns::doAttention(canvas, tickMs); break; \
    } \
  } while (0)

void catDrawFrame(M5Canvas* canvas, Persona p, uint32_t tickMs) {
  switch (g_kind) {
    case BUDDY_OWL:     DISPATCH(owl);     return;
    case BUDDY_DUCK:    DISPATCH(duck);    return;
    case BUDDY_PENGUIN: DISPATCH(penguin); return;
    case BUDDY_RABBIT:  DISPATCH(rabbit);  return;
    case BUDDY_DRAGON:  DISPATCH(dragon);  return;
    case BUDDY_GHOST:   DISPATCH(ghost);   return;
    case BUDDY_ROBOT:   DISPATCH(robot);   return;
    case BUDDY_CAT:
    default:
      // cat functions are in the anonymous top-level scope of this file
      switch (p) {
        case P_SLEEP:     doSleep(canvas, tickMs); break;
        case P_IDLE:      doIdle(canvas, tickMs); break;
        case P_BUSY:      doBusy(canvas, tickMs); break;
        case P_ATTENTION: doAttention(canvas, tickMs); break;
      }
      return;
  }
}
