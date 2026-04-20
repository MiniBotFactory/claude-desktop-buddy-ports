// NimBLE-based Nordic UART Service.
//
// Mirrors the behaviour of the Arduino `ble_bridge.cpp` used on the
// CoreS3/AtomS3R ports — same Service/RX/TX UUIDs, same SM_PASSKEY_DISP
// capability, same newline-delimited JSON contract at the wire. The only
// reason this exists is that ESP-IDF doesn't ship Arduino's BLEDevice API,
// so we use NimBLE (the ESP-IDF native BLE host stack).

#include "ble_nus.h"

#include <string.h>
#include <stdio.h>

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

static const char *TAG = "ble";

// Nordic UART Service UUIDs.
//   6e400001-b5a3-f393-e0a9-e50e24dcca9e   (service)
//   6e400002-b5a3-f393-e0a9-e50e24dcca9e   (RX, peer→us, write)
//   6e400003-b5a3-f393-e0a9-e50e24dcca9e   (TX, us→peer, notify)
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static bool     s_notify_enabled = false;
static bool     s_connected = false;
static bool     s_secure = false;
static uint32_t s_passkey = 0;
static uint8_t  s_addr_type = 0;
static char     s_name[32] = {0};

// RX ring buffer (central writes → we buffer until caller reads).
#define RX_CAP 2048
static uint8_t  s_rx[RX_CAP];
static volatile size_t s_rx_head = 0, s_rx_tail = 0;

static void rx_push(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t next = (s_rx_head + 1) % RX_CAP;
        if (next == s_rx_tail) return;  // full, drop
        s_rx[s_rx_head] = p[i];
        s_rx_head = next;
    }
}

size_t ble_nus_available(void) {
    return (s_rx_head + RX_CAP - s_rx_tail) % RX_CAP;
}

size_t ble_nus_read(uint8_t *dst, size_t max) {
    size_t n = 0;
    while (n < max && s_rx_head != s_rx_tail) {
        dst[n++] = s_rx[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % RX_CAP;
    }
    return n;
}

bool ble_nus_connected(void) { return s_connected; }
bool ble_nus_secure(void)    { return s_secure; }
uint32_t ble_nus_passkey(void) { return s_passkey; }

size_t ble_nus_write(const uint8_t *data, size_t len) {
    if (!s_connected || !s_notify_enabled || s_tx_val_handle == 0) return 0;
    // Chunk to MTU-3 for ATT notifications.
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    size_t chunk = (mtu > 3) ? (mtu - 3) : 20;
    if (chunk > 180) chunk = 180;
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > chunk) n = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + sent, n);
        if (!om) break;
        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        if (rc != 0) break;
        sent += n;
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    return sent;
}

// ---- GATT access callback ---------------------------------------------------
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        size_t plen = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[256];
        while (plen > 0) {
            uint16_t n = (plen > sizeof(buf)) ? sizeof(buf) : plen;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, n, NULL);
            if (rc != 0) break;
            rx_push(buf, n);
            plen -= n;
        }
    }
    return 0;
}

static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // TX is notify-only; no direct reads/writes expected.
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &NUS_RX_UUID.u,
                .access_cb = nus_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            }, {
                .uuid = &NUS_TX_UUID.u,
                .access_cb = nus_tx_access,
                .flags = BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN,
                .val_handle = &s_tx_val_handle,
            }, { 0 }
        },
    },
    { 0 }
};

// Forward-declared because advertise() passes it to ble_gap_adv_start.
static int gap_event(struct ble_gap_event *event, void *arg);

// ---- Advertising ------------------------------------------------------------
static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGW(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "adv_start rc=%d", rc);
    else         ESP_LOGI(TAG, "advertising as '%s'", s_name);
}

// ---- GAP event callback -----------------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
            // Ask the central to bring encryption up so our encrypted chars
            // become accessible.
            ble_gap_security_initiate(s_conn_handle);
        } else {
            ESP_LOGI(TAG, "connect failed status=%d", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_secure = false;
        s_passkey = 0;
        s_notify_enabled = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // Generate a random 6-digit code, store for UI, return to stack.
            uint32_t r = esp_random();
            s_passkey = r % 1000000;
            pkey.action = BLE_SM_IOACT_DISP;
            pkey.passkey = s_passkey;
            ESP_LOGI(TAG, "passkey %06lu", (unsigned long)s_passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_secure = (event->enc_change.status == 0);
        if (s_secure) {
            s_passkey = 0;
            ESP_LOGI(TAG, "auth ok");
        } else {
            ESP_LOGW(TAG, "auth FAIL status=%d", event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notify=%d", s_notify_enabled);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", event->mtu.value);
        break;
    }
    return 0;
}

// ---- Host task + on-sync -----------------------------------------------------
static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_nus_clear_bonds(void) {
    ble_store_clear();
    ESP_LOGI(TAG, "bonds cleared");
}

void ble_nus_init(const char *device_name) {
    if (device_name && *device_name) {
        strncpy(s_name, device_name, sizeof(s_name) - 1);
    } else {
        strcpy(s_name, "Claude Note4");
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;

    // LE Secure Connections, passkey display on us.
    ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_mitm    = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set(s_name);

    // Tap every GAP event through our handler once connections arrive.
    // NimBLE requires passing the callback at advertising start (see advertise()),
    // but we also want it during listener mode; register globally.
    // NOTE: older NimBLE versions route all events via the adv-start callback;
    // the cb in advertise() above uses gap_event.
    // To be safe we also set a default event handler via ble_gap_adv_start
    // above.

    nimble_port_freertos_init(nimble_host_task);
}
