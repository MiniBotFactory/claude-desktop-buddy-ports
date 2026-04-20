#pragma once
#include <M5Unified.h>
#include <stdint.h>

enum Persona : uint8_t {
  P_SLEEP = 0,
  P_IDLE = 1,
  P_BUSY = 2,
  P_ATTENTION = 3,
};

enum BuddyKind : uint8_t {
  BUDDY_CAT = 0,
  BUDDY_OWL = 1,
  BUDDY_COUNT,
};

// Pick which species renders next. Persists until the next call.
void buddySetKind(BuddyKind k);
BuddyKind buddyGetKind(void);
const char *buddyKindName(BuddyKind k);

// Draw one frame of the current buddy into `canvas` at origin (0,0).
// `tickMs` advances animation; call every loop with millis().
// `canvas` must be at least 144x96 px, cleared or background-filled before call.
void catDrawFrame(M5Canvas* canvas, Persona p, uint32_t tickMs);
