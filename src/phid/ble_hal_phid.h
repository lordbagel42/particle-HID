/*
 * BLE security delegate extension ("PHID ABI") for nRF52840 Device OS.
 *
 * Lets user firmware own bonding: it decides whether to bond and where the
 * exchanged keys go, answers LTK requests from returning hosts, and restores
 * per-host system attributes (CCCDs). Without a delegate the HAL behaves
 * exactly like stock Device OS.
 *
 * All calls go through hal_ble_internal(op, data, size, nullptr). Stock
 * Device OS returns SYSTEM_ERROR_NOT_SUPPORTED, which is how callers detect
 * the extension.
 *
 * Keep this file identical to particle-hid's src/phid/ble_hal_phid.h.
 */

#ifndef BLE_HAL_PHID_H
#define BLE_HAL_PHID_H

#include <stdint.h>

#define PHID_ABI_VERSION 1

#define PHID_OP_GET_INFO     0x50480001
#define PHID_OP_SET_DELEGATE 0x50480002

#define PHID_CAP_SEC_DELEGATE      (1u << 0) // SEC_PARAMS hook and consumable SEC_INFO_REQUEST
#define PHID_CAP_SYS_ATTR_DELEGATE (1u << 1) // consumable BLE_GATTS_EVT_SYS_ATTR_MISSING
#define PHID_CAP_TX_WAKE           (1u << 2) // wake semaphore given on BLE_GATTS_EVT_HVN_TX_COMPLETE
#define PHID_CAP_SC_CONFIRM        (1u << 3) // BLE_GATTS_EVT_SC_CONFIRM delivered to the delegate

typedef struct phid_info_t {
    uint16_t version;
    uint16_t size;
    uint32_t abi_version;
    uint32_t caps;
} phid_info_t;

// Delegate operations.
#define PHID_DELEGATE_EVENT      1 // data: const ble_evt_t*
#define PHID_DELEGATE_SEC_PARAMS 2 // data: phid_sec_params_t*

/*
 * PHID_DELEGATE_SEC_PARAMS is called on the BLE event thread right before the
 * HAL replies to BLE_GAP_EVT_SEC_PARAMS_REQUEST on a peripheral link. The
 * delegate sets reject or bond. When bond is set, the key pointers must stay
 * valid until BLE_GAP_EVT_AUTH_STATUS for this connection.
 */
typedef struct phid_sec_params_t {
    uint16_t version;
    uint16_t size;
    uint16_t conn_handle;
    uint8_t reject;           // out: reply PAIRING_NOT_SUPP
    uint8_t bond;             // out: request bonding and distribute keys
    const void* peer_params;  // in: const ble_gap_sec_params_t*
    void* own_enc;            // out: ble_gap_enc_key_t*
    void* own_id;             // out: ble_gap_id_key_t* (normally NULL)
    void* peer_enc;           // out: ble_gap_enc_key_t*
    void* peer_id;            // out: ble_gap_id_key_t*
} phid_sec_params_t;

/*
 * Called on the BLE event thread, never from an interrupt.
 *
 * PHID_DELEGATE_EVENT receives every event the HAL dispatches to its thread,
 * for all roles. Two event types are consumable and are delivered before the
 * HAL's default handling; return 1 when the delegate replied to them:
 *   - BLE_GAP_EVT_SEC_INFO_REQUEST (default: no reply, as in stock Device OS)
 *   - BLE_GATTS_EVT_SYS_ATTR_MISSING (default: empty system attributes)
 * All other events are delivered after the HAL's handling; the return value
 * is ignored.
 *
 * The callback must not block and must not call hal_ble_* functions, which
 * take the BLE API lock that application threads may hold while they wait
 * on this thread. Calling sd_* SoftDevice functions is allowed.
 */
typedef int (*phid_delegate_cb_t)(int op, void* data, void* context);

typedef struct phid_delegate_t {
    uint16_t version;
    uint16_t size;
    phid_delegate_cb_t callback; // NULL removes the delegate
    void* context;
    void* wake_semaphore;        // os_semaphore_t, optional; given on every HVN_TX_COMPLETE
} phid_delegate_t;

#endif // BLE_HAL_PHID_H
