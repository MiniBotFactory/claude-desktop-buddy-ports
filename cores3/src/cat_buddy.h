#pragma once
#include <M5Unified.h>
#include <stdint.h>

enum Persona : uint8_t {
  P_SLEEP = 0,
  P_IDLE = 1,
  P_BUSY = 2,
  P_ATTENTION = 3,
};

// Draw one frame of the cat into `canvas` at origin (0,0).
// `tickMs` advances animation; call every loop with millis().
// `canvas` must be at least 144x96 px, cleared or background-filled before call.
void catDrawFrame(M5Canvas* canvas, Persona p, uint32_t tickMs);
