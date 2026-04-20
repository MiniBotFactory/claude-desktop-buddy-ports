// Hardware Buddy JSON snapshot → typed state, same contract as the
// CoreS3/AtomS3R ports. Uses ESP-IDF's builtin cJSON.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t  total;
    uint8_t  running;
    uint8_t  waiting;
    uint32_t tokens_today;
    char     msg[48];
    char     prompt_id[48];
    char     prompt_tool[24];
    char     prompt_hint[80];
    uint32_t last_update_ms;
} proto_snapshot_t;

typedef enum {
    PERSONA_SLEEP     = 0,
    PERSONA_IDLE      = 1,
    PERSONA_BUSY      = 2,
    PERSONA_ATTENTION = 3,
} persona_t;

#ifdef __cplusplus
extern "C" {
#endif

// Feed one line of inbound JSON into the state. Returns true if the
// snapshot was modified (so the UI should redraw).
bool proto_apply(proto_snapshot_t *s, const char *line);

// Map a snapshot to a persona, using a 30s online window like the other
// ports (tolerates macOS BLE cycling without putting the device to sleep).
persona_t proto_persona(const proto_snapshot_t *s);

// Format an outbound permission reply. Buf must be at least 128 bytes.
int proto_fmt_permission(char *buf, size_t buflen,
                         const char *id, const char *decision);

#ifdef __cplusplus
}
#endif
