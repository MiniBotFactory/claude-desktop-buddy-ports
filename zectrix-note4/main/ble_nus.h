// Nordic UART Service over NimBLE, Hardware Buddy-compatible.
//
// Service 6e400001-b5a3-f393-e0a9-e50e24dcca9e  (same as ble_bridge.cpp on
// the CoreS3/AtomS3R ports — Claude Desktop looks for this UUID). RX and
// TX characteristics are encrypted-only; pairing uses LE Secure Connections
// with DisplayOnly IO capability, so the host types the 6-digit passkey
// the user reads off our e-paper.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NimBLE stack + NUS service and start advertising.
// `device_name` is advertised; start it with "Claude" so the desktop's
// Hardware Buddy scanner picks it up.
void ble_nus_init(const char *device_name);

// True whenever a central is connected (secure or not).
bool ble_nus_connected(void);

// True once LE Secure Connections pairing has completed for this link.
bool ble_nus_secure(void);

// Non-zero while a 6-digit passkey needs to be displayed. Cleared on
// auth complete or disconnect.
uint32_t ble_nus_passkey(void);

// Drop every stored bond from NVS. After this you must re-pair from the
// desktop side as well.
void ble_nus_clear_bonds(void);

// Read up to `max` bytes from the RX ring buffer. Returns actual count.
size_t ble_nus_read(uint8_t *dst, size_t max);

// Bytes currently waiting in the RX ring.
size_t ble_nus_available(void);

// Notify data to the central. Chunks to MTU internally.
size_t ble_nus_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
