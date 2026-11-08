#include "ble_hid.h"

#include <cstring>

#include "bonds.h"
#include "os_install.h"
#include "platform.h"

#if defined(PARTICLE)
#include "Particle.h" // defines HAL_PLATFORM_NRF52840
#endif

#if defined(PARTICLE) && HAL_PLATFORM_NRF52840

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ble.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_hal.h"
#include "ble_hal_phid.h"
#include "concurrent_hal.h"
#include "nrf_soc.h"
#include "system_error.h"

namespace phid {

namespace detail {
extern volatile os_semaphore_t wakeSemaphore; // platform_particle.cpp
} // namespace detail

namespace {

Logger hidLog("hid");

constexpr uint16_t kInvalidConn = BLE_CONN_HANDLE_INVALID;
constexpr size_t kMaxDevices = HidEngine::kMaxDevices;
constexpr size_t kMaxReport = HidEngine::kMaxReport;
constexpr uint32_t kSecurityRequestDelayMs = 1500;
constexpr uint32_t kSubscriptionPollMs = 500;
constexpr uint32_t kBatteryPollMs = 30000;
constexpr uint32_t kAdvCheckMs = 3000;
constexpr uint32_t kStoreRetryMs = 1000;
constexpr uint32_t kScRetryMs = 500;
constexpr uint8_t kScMaxTries = 10;
constexpr const char* kBondDir = "/usr/phid";

enum class Ev : uint8_t {
    Connected,
    Disconnected,
    Secured,
    SecurityFailed,
    PairingRejected,
    Write,
    CccdChanged,
    ServiceChanged,
};

struct WorkerEvent {
    Ev type;
    uint8_t code;  // disconnect reason, security status
    int8_t slot;   // bond slot
    uint8_t flag;  // Secured: new bond
    uint16_t handle;
    uint8_t len;
    uint8_t data[4];
};

struct Link {
    uint16_t conn = kInvalidConn;
    uint8_t peerType = 0;
    uint8_t peerAddr[6] = {};
    int8_t slot = -1;    // bond recognized from the peer address
    int8_t encSlot = -1; // bond whose key encrypted the link
    uint8_t secLevel = 1;
    bool pairing = false;
    bool secReqSent = false;
    bool scPending = false; // Service Changed owed to this host, committed on SC_CONFIRM
    bool scSent = false;
    uint8_t scTries = 0;
    uint32_t scRetryAt = 0;
    uint32_t connectedAt = 0;
};

struct PendingKeys {
    ble_gap_enc_key_t ownEnc;
    ble_gap_enc_key_t peerEnc;
    ble_gap_id_key_t peerId;
};

struct GattHandles {
    uint16_t first;
    uint16_t last;
    uint16_t protocolMode;
    uint16_t controlPoint;
    uint16_t reportMap;
    uint16_t input[kMaxDevices];
    uint16_t inputCccd[kMaxDevices];
    uint16_t kbOutput;
    uint16_t bootKbIn;
    uint16_t bootKbInCccd;
    uint16_t bootKbOut;
    uint16_t bootMouseIn;
    uint16_t bootMouseInCccd;
    uint16_t battery;
    uint16_t batteryCccd;
};

struct Config {
    char name[21] = "Particle HID";
    char manufacturer[32] = "Particle";
    char model[32] = "Boron HID";
    uint16_t appearance = 0;
    uint8_t pnp[7] = {0x02, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01}; // USB-IF, VID 0x1209, PID 0x0001, v1.00
    ConnectionPreset preset = ConnectionPreset::Balanced;
    uint8_t maxBonds = 4;
    bool evict = true;
    bool autoPair = true;
    BatterySource battery = BatterySource::Auto;
    uint32_t readyDeadline = 2000;
    bool allowUnbonded = false;
    bool installPatch = true;
};

bool sdAes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    nrf_ecb_hal_data_t d = {};
    memcpy(d.key, key, 16);
    memcpy(d.cleartext, in, 16);
    if (sd_ecb_block_encrypt(&d) != NRF_SUCCESS) {
        return false;
    }
    memcpy(out, d.ciphertext, 16);
    return true;
}

void secMode(ble_gap_conn_sec_mode_t* m, bool secure) {
    if (secure) {
        BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(m);
    } else {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(m);
    }
}

enum Prop : uint8_t { R = 1, W = 2, WNR = 4, N = 8 };

uint32_t addChar(uint16_t svc, uint16_t uuid, uint8_t props, uint8_t* value, uint16_t len, uint16_t maxLen, bool secure,
        ble_gatts_char_handles_t* h) {
    ble_gatts_char_md_t md = {};
    md.char_props.read = (props & R) != 0;
    md.char_props.write = (props & W) != 0;
    md.char_props.write_wo_resp = (props & WNR) != 0;
    md.char_props.notify = (props & N) != 0;
    ble_gatts_attr_md_t cccd = {};
    if (props & N) {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd.read_perm);
        secMode(&cccd.write_perm, secure);
        cccd.vloc = BLE_GATTS_VLOC_STACK;
        md.p_cccd_md = &cccd;
    }
    ble_gatts_attr_md_t am = {};
    if (props & R) {
        secMode(&am.read_perm, secure);
    } else {
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.read_perm);
    }
    if (props & (W | WNR)) {
        secMode(&am.write_perm, secure);
    } else {
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.write_perm);
    }
    am.vloc = BLE_GATTS_VLOC_USER;
    am.vlen = 0;
    ble_uuid_t u = {uuid, BLE_UUID_TYPE_BLE};
    ble_gatts_attr_t a = {};
    a.p_uuid = &u;
    a.p_attr_md = &am;
    a.init_len = len;
    a.init_offs = 0;
    a.max_len = maxLen;
    a.p_value = value;
    return sd_ble_gatts_characteristic_add(svc, &md, &a, h);
}

uint32_t addReportRef(uint8_t id, uint8_t type, bool secure) {
    uint8_t v[2] = {id, type};
    ble_gatts_attr_md_t am = {};
    secMode(&am.read_perm, secure);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.write_perm);
    am.vloc = BLE_GATTS_VLOC_STACK;
    ble_uuid_t u = {0x2908, BLE_UUID_TYPE_BLE};
    ble_gatts_attr_t a = {};
    a.p_uuid = &u;
    a.p_attr_md = &am;
    a.init_len = 2;
    a.max_len = 2;
    a.p_value = v;
    uint16_t h;
    return sd_ble_gatts_descriptor_add(BLE_GATT_HANDLE_INVALID, &a, &h);
}

uint32_t addService(uint16_t uuid, uint16_t* h) {
    ble_uuid_t u = {uuid, BLE_UUID_TYPE_BLE};
    return sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &u, h);
}

} // namespace

struct BleHidImpl {
    BleHidClass* owner = &BleHid;
    Config cfg;
    bool started = false;
    bool gattBuilt = false;
    bool patched = false;
    bool secure = false; // attributes need encryption (bonding mode)
    HidReason error = HidReason::None;

    // Shared with the BLE thread; guarded by platform::lock().
    BondTable bonds;
    Link link;
    PendingKeys pending = {};
    WorkerEvent events[16];
    size_t eventCount = 0;
    bool overflow = false;
    uint8_t dirtyBonds = 0;
    uint8_t deleteBonds = 0;
    uint32_t profileBase = 0; // report map + PnP ID
    uint32_t profileHash = 0; // profileBase + every attribute in the GATT table
    uint16_t appFirstHandle = 0; // first service after the SoftDevice's GAP and GATT services
    bool pairingOpen = false;
    bool pairingForever = false;
    uint32_t pairingUntil = 0;
    bool disconnectRequested = false;
    HidStatus status;
    BleHidStats stats;
    uint8_t batteryRequested = 0xFF;

    // Worker only.
    os_thread_t thread = nullptr;
    os_semaphore_t wake = nullptr;
    volatile bool stopping = false;
    volatile bool exited = false;
    GattHandles gatt = {};
    bool subsDirty = true;
    uint32_t lastSubsPoll = 0;
    uint32_t advStartedAt = 0;
    int advPhase = -1;
    bool advDirty = true;
    bool advDiscoverable = false;
    uint32_t lastAdvCheck = 0;
    uint32_t lastBattery = 0;
    uint32_t lastStoreAttempt = 0;
    uint32_t lastLinkPoll = 0;
    bool suspended = false;
    void (*statusCallback)(const HidStatus&) = nullptr;
    HidStatus lastReported;

    // GATT value storage (VLOC_USER: the SoftDevice reads and writes these in place).
    uint8_t inputBuf[kMaxDevices][kMaxReport] = {};
    uint8_t kbOutBuf[1] = {};
    uint8_t bootKbIn[8] = {};
    uint8_t bootKbOut[1] = {};
    uint8_t bootMouseIn[3] = {};
    uint8_t protocolMode[1] = {1}; // report protocol
    uint8_t controlPoint[1] = {};
    uint8_t hidInfo[4] = {0x11, 0x01, 0x00, 0x02}; // HID 1.11, no country, normally connectable
    uint8_t batteryBuf[1] = {100};
    uint8_t pnp[7] = {};
    char manufacturer[32] = {};
    char model[32] = {};

    struct Transport : phid::Transport {
        BleHidImpl* impl;
        SendResult sendInput(size_t i, bool boot, const uint8_t* data, size_t len) override {
            return impl->send(i, boot, data, len);
        }
    } transport;

    // ---- shared helpers (lock held) ----

    void post(const WorkerEvent& e) {
        if (eventCount == sizeof(events) / sizeof(events[0])) {
            overflow = true;
            return;
        }
        events[eventCount++] = e;
    }

    void captureSysAttrs(Bond& b, uint16_t conn) {
        uint8_t sys[Bond::kSysAttrMax];
        uint8_t usr[Bond::kUsrAttrMax];
        uint16_t sysLen = sizeof(sys);
        uint16_t usrLen = sizeof(usr);
        uint32_t r1 = sd_ble_gatts_sys_attr_get(conn, sys, &sysLen, BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS);
        uint32_t r2 = sd_ble_gatts_sys_attr_get(conn, usr, &usrLen, BLE_GATTS_SYS_ATTR_FLAG_USR_SRVCS);
        if (r1 == NRF_ERROR_NOT_FOUND) {
            sysLen = 0;
            r1 = NRF_SUCCESS;
        }
        if (r2 == NRF_ERROR_NOT_FOUND) {
            usrLen = 0;
            r2 = NRF_SUCCESS;
        }
        if (r1 != NRF_SUCCESS || r2 != NRF_SUCCESS) {
            return;
        }
        const int slot = (int)(&b - &bonds.slot(0));
        if (b.sysLen != sysLen || b.usrLen != usrLen || memcmp(b.sys, sys, sysLen) || memcmp(b.usr, usr, usrLen)) {
            b.sysLen = sysLen;
            b.usrLen = usrLen;
            memcpy(b.sys, sys, sysLen);
            memcpy(b.usr, usr, usrLen);
            dirtyBonds |= 1 << slot;
        }
    }

    // Gives the host back its subscriptions. When our profile changed since the
    // host cached it (or its saved subscriptions no longer fit our table), only the
    // system part (Service Changed CCCD) is restored and the host is told to rediscover.
    void restoreSysAttrs(int slot, uint16_t conn) {
        Bond& b = bonds.slot(slot);
        bool current = b.profileHash == profileHash;
        if (sd_ble_gatts_sys_attr_set(conn, b.sysLen ? b.sys : nullptr, b.sysLen, BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS) != NRF_SUCCESS) {
            sd_ble_gatts_sys_attr_set(conn, nullptr, 0, BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS);
        }
        // Restore the host's subscriptions even when the table changed, as nRF5 Peer
        // Manager does: a host that ignores Service Changed keeps working as long as
        // our CCCDs didn't move. The SoftDevice rejects a blob that no longer fits.
        if (sd_ble_gatts_sys_attr_set(conn, b.usrLen ? b.usr : nullptr, b.usrLen, BLE_GATTS_SYS_ATTR_FLAG_USR_SRVCS) != NRF_SUCCESS) {
            sd_ble_gatts_sys_attr_set(conn, nullptr, 0, BLE_GATTS_SYS_ATTR_FLAG_USR_SRVCS);
            current = false;
        }
        if (!current) {
            if (!link.scPending) {
                link.scPending = true;
                link.scSent = false;
                link.scTries = 0;
                sendServiceChanged();
            }
        }
    }

    // The bond only takes the new profile hash once the host confirmed Service
    // Changed; until then every reconnect sends it again.
    void sendServiceChanged() {
        if (!link.scPending || link.scSent || link.encSlot < 0) {
            return;
        }
        const uint32_t r = sd_ble_gatts_service_changed(link.conn, appFirstHandle ? appFirstHandle : gatt.first, 0xFFFF);
        stats.scLastResult = r;
        if (r == NRF_SUCCESS) {
            link.scSent = true;
        } else if ((r == NRF_ERROR_INVALID_STATE || r == NRF_ERROR_BUSY) && ++link.scTries < kScMaxTries) {
            link.scRetryAt = platform::millis() + kScRetryMs; // MTU exchange or indication in flight
        } else {
            // Still refused: the host never enabled Service Changed, so it doesn't cache our table.
            commitProfile();
        }
    }

    void commitProfile() {
        Bond& b = bonds.slot(link.encSlot);
        link.scPending = false;
        if (!b.used) {
            return; // forgotten meanwhile
        }
        b.profileHash = profileHash;
        captureSysAttrs(b, link.conn); // the subscriptions in effect now
        dirtyBonds |= 1 << link.encSlot;
        post(WorkerEvent{Ev::ServiceChanged, 0, link.encSlot, 0, 0, 0, {}});
    }

    // ---- BLE event thread (delegate) ----

    static int delegate(int op, void* data, void* ctx) {
        auto self = static_cast<BleHidImpl*>(ctx);
        if (!self) {
            return 0; // raced with removal in end()
        }
        platform::Guard g;
        int r = 0;
        if (op == PHID_DELEGATE_SEC_PARAMS) {
            self->onSecParams(static_cast<phid_sec_params_t*>(data));
        } else if (op == PHID_DELEGATE_EVENT) {
            r = self->onEvent(static_cast<const ble_evt_t*>(data));
        }
        if (self->wake) {
            os_semaphore_give(self->wake, false);
        }
        return r;
    }

    void onSecParams(phid_sec_params_t* p) {
        if (p->conn_handle != link.conn) {
            return; // not our peripheral link; HAL default
        }
        const bool known = link.slot >= 0;
        bool full = bonds.count() >= bonds.capacity();
        if (!pairingOpen) {
            p->reject = 1;
            ++stats.pairingRejected;
            post(WorkerEvent{Ev::PairingRejected, 0, link.slot, 0, 0, 0, {}});
            return;
        }
        if (full && !known && !cfg.evict) {
            p->reject = 1; // bond table full and eviction disabled
            ++stats.pairingRejected;
            post(WorkerEvent{Ev::PairingRejected, 1, -1, 0, 0, 0, {}});
            return;
        }
        pending = {};
        p->bond = 1;
        p->own_enc = &pending.ownEnc;
        p->own_id = nullptr;
        p->peer_enc = &pending.peerEnc;
        p->peer_id = &pending.peerId;
        link.pairing = true;
    }

    int onEvent(const ble_evt_t* e) {
        switch (e->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED: {
            const auto& c = e->evt.gap_evt.params.connected;
            if (c.role != BLE_GAP_ROLE_PERIPH) {
                return 0;
            }
            refreshProfileHash(); // before any bond compares against it
            link = Link();
            link.conn = e->evt.gap_evt.conn_handle;
            link.peerType = c.peer_addr.addr_type;
            memcpy(link.peerAddr, c.peer_addr.addr, 6);
            link.slot = (int8_t)bonds.findByAddress(link.peerType, link.peerAddr, sdAes);
            link.connectedAt = platform::millis();
            post(WorkerEvent{Ev::Connected, 0, link.slot, 0, 0, 0, {}});
            return 0;
        }
        case BLE_GAP_EVT_DISCONNECTED: {
            if (e->evt.gap_evt.conn_handle != link.conn) {
                return 0;
            }
            // The HAL drops GATTS_WRITE events when its pool is empty; take the final
            // subscriptions now (the SoftDevice keeps them until the handle is reused).
            if (link.encSlot >= 0 && link.secLevel >= 2) {
                captureSysAttrs(bonds.slot(link.encSlot), link.conn);
            }
            post(WorkerEvent{Ev::Disconnected, e->evt.gap_evt.params.disconnected.reason, link.slot, 0, 0, 0, {}});
            link = Link();
            return 0;
        }
        case BLE_GAP_EVT_SEC_INFO_REQUEST: {
            const uint16_t conn = e->evt.gap_evt.conn_handle;
            if (conn != link.conn) {
                return 0;
            }
            const auto& req = e->evt.gap_evt.params.sec_info_request;
            int slot = bonds.findByMasterId(req.master_id.ediv, req.master_id.rand);
            if (slot < 0) {
                slot = link.slot >= 0 ? link.slot : bonds.findByAddress(req.peer_addr.addr_type, req.peer_addr.addr, sdAes);
            }
            const Bond* b = slot >= 0 ? &bonds.slot(slot) : nullptr;
            // LESC keys have no master ID; legacy keys must match theirs (as nRF5 Peer Manager does).
            if (b && req.enc_info && (b->lesc || (b->ediv == req.master_id.ediv && memcmp(b->rand, req.master_id.rand, 8) == 0))) {
                ble_gap_enc_info_t info = {};
                memcpy(info.ltk, b->ltk, 16);
                info.lesc = b->lesc;
                info.auth = b->authenticated;
                info.ltk_len = b->ltkLen;
                link.encSlot = (int8_t)slot;
                sd_ble_gap_sec_info_reply(conn, &info, nullptr, nullptr);
            } else {
                link.encSlot = -1;
                sd_ble_gap_sec_info_reply(conn, nullptr, nullptr, nullptr); // key missing: the host re-pairs
            }
            return 1;
        }
        case BLE_GAP_EVT_CONN_SEC_UPDATE: {
            if (e->evt.gap_evt.conn_handle != link.conn) {
                return 0;
            }
            const auto& sm = e->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode;
            link.secLevel = sm.sm == 1 ? sm.lv : 1;
            if (!link.pairing && link.secLevel >= 2 && link.encSlot >= 0) {
                link.slot = link.encSlot;
                restoreSysAttrs(link.encSlot, link.conn);
                bonds.touch(link.encSlot);
                dirtyBonds |= 1 << link.encSlot;
                post(WorkerEvent{Ev::Secured, 0, link.encSlot, 0, 0, 0, {}});
            }
            return 0;
        }
        case BLE_GAP_EVT_AUTH_STATUS: {
            if (e->evt.gap_evt.conn_handle != link.conn) {
                return 0;
            }
            const auto& a = e->evt.gap_evt.params.auth_status;
            const bool wasPairing = link.pairing;
            link.pairing = false;
            if (a.auth_status != BLE_GAP_SEC_STATUS_SUCCESS) {
                ++stats.securityFailures;
                stats.lastSecurityStatus = a.auth_status;
                post(WorkerEvent{Ev::SecurityFailed, a.auth_status, link.slot, 0, 0, 0, {}});
                return 0;
            }
            if (!wasPairing || !a.bonded) {
                post(WorkerEvent{Ev::Secured, 0, -1, 0, 0, 0, {}}); // encrypted, but the host chose not to bond
                return 0;
            }
            storeNewBond(a.kdist_peer.id);
            return 0;
        }
        case BLE_GATTS_EVT_SYS_ATTR_MISSING: {
            const uint16_t conn = e->evt.gatts_evt.conn_handle;
            if (conn != link.conn) {
                return 0;
            }
            if (link.secLevel >= 2 && link.encSlot >= 0) {
                restoreSysAttrs(link.encSlot, conn);
            } else {
                sd_ble_gatts_sys_attr_set(conn, nullptr, 0, 0);
            }
            return 1;
        }
        case BLE_GATTS_EVT_WRITE: {
            const uint16_t conn = e->evt.gatts_evt.conn_handle;
            if (conn != link.conn) {
                return 0;
            }
            const auto& w = e->evt.gatts_evt.params.write;
            WorkerEvent ev = {Ev::Write, 0, link.slot, 0, w.handle, (uint8_t)(w.len > 4 ? 4 : w.len), {}};
            memcpy(ev.data, w.data, ev.len);
            if (w.handle == gatt.protocolMode || w.handle == gatt.controlPoint || w.handle == gatt.kbOutput || w.handle == gatt.bootKbOut) {
                post(ev);
            } else {
                if (w.uuid.type == BLE_UUID_TYPE_BLE && w.uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG) {
                    ++stats.cccdWrites;
                }
                ev.type = Ev::CccdChanged; // a CCCD (ours or Service Changed), or an app characteristic
                post(ev);
            }
            if (link.encSlot >= 0 && link.secLevel >= 2) {
                captureSysAttrs(bonds.slot(link.encSlot), conn);
            }
            return 0;
        }
        case BLE_GATTS_EVT_SC_CONFIRM:
            if (e->evt.gatts_evt.conn_handle == link.conn && link.scPending) {
                ++stats.scConfirmed;
                commitProfile();
            }
            return 0;
        default:
            return 0;
        }
    }

    void storeNewBond(bool peerSentId) {
        if (pending.ownEnc.enc_info.ltk_len == 0) {
            // A legacy host that didn't ask for our key can't re-encrypt; nothing worth storing.
            post(WorkerEvent{Ev::Secured, 0, -1, 0, 0, 0, {}});
            return;
        }
        uint8_t type = link.peerType;
        uint8_t addr[6];
        memcpy(addr, link.peerAddr, 6);
        if (peerSentId) {
            type = pending.peerId.id_addr_info.addr_type;
            memcpy(addr, pending.peerId.id_addr_info.addr, 6);
        }
        const int slot = bonds.allocate(type, addr, cfg.evict);
        if (slot < 0) {
            post(WorkerEvent{Ev::Secured, 0, -1, 0, 0, 0, {}});
            return;
        }
        Bond b;
        b.used = true;
        b.addrType = type;
        memcpy(b.addr, addr, 6);
        b.hasIrk = peerSentId;
        memcpy(b.irk, pending.peerId.id_info.irk, 16);
        const ble_gap_enc_info_t& enc = pending.ownEnc.enc_info;
        memcpy(b.ltk, enc.ltk, 16);
        b.ltkLen = enc.ltk_len;
        b.lesc = enc.lesc;
        b.authenticated = enc.auth;
        b.ediv = pending.ownEnc.master_id.ediv;
        memcpy(b.rand, pending.ownEnc.master_id.rand, 8);
        b.profileHash = profileHash;
        bonds.slot(slot) = b;
        bonds.touch(slot);
        captureSysAttrs(bonds.slot(slot), link.conn);
        dirtyBonds |= 1 << slot;
        deleteBonds &= ~(1 << slot);
        link.slot = (int8_t)slot;
        link.encSlot = (int8_t)slot;
        ++stats.newBonds;
        post(WorkerEvent{Ev::Secured, 0, (int8_t)slot, 1, 0, 0, {}});
    }

    // ---- worker ----

    SendResult send(size_t i, bool boot, const uint8_t* data, size_t len) {
        if (link.conn == kInvalidConn) {
            return SendResult::NotSubscribed;
        }
        uint16_t handle = gatt.input[i];
        if (boot) {
            handle = owner->engine().device(i).bootRole() == BootRole::BootKeyboard ? gatt.bootKbIn : gatt.bootMouseIn;
        }
        uint16_t n = (uint16_t)len;
        ble_gatts_hvx_params_t p = {};
        p.handle = handle;
        p.type = BLE_GATT_HVX_NOTIFICATION;
        p.p_len = &n;
        p.p_data = data;
        switch (sd_ble_gatts_hvx(link.conn, &p)) {
        case NRF_SUCCESS:
            return SendResult::Ok;
        case NRF_ERROR_RESOURCES:
        case NRF_ERROR_BUSY:
            return SendResult::Busy;
        case NRF_ERROR_INVALID_STATE:
        case BLE_ERROR_GATTS_SYS_ATTR_MISSING:
            subsDirty = true;
            return SendResult::NotSubscribed;
        default:
            return SendResult::Error;
        }
    }

    bool cccdEnabled(uint16_t conn, uint16_t cccd) {
        uint8_t v[2] = {};
        ble_gatts_value_t gv = {};
        gv.len = 2;
        gv.p_value = v;
        return cccd && sd_ble_gatts_value_get(conn, cccd, &gv) == NRF_SUCCESS && (v[0] & BLE_GATT_HVX_NOTIFICATION);
    }

    void pollSubscriptions() {
        HidEngine& e = owner->engine();
        for (size_t i = 0; i < e.deviceCount(); ++i) {
            uint16_t cccd = gatt.inputCccd[i];
            if (e.bootProtocol()) {
                const BootRole role = e.device(i).bootRole();
                cccd = role == BootRole::BootKeyboard ? gatt.bootKbInCccd : role == BootRole::BootMouse ? gatt.bootMouseInCccd : 0;
            }
            e.setSubscribed(i, link.conn != kInvalidConn && cccdEnabled(link.conn, cccd));
        }
    }

    void handleWrite(const WorkerEvent& ev) {
        HidEngine& e = owner->engine();
        if (ev.handle == gatt.protocolMode && ev.len >= 1) {
            e.setBootProtocol(ev.data[0] == 0);
            subsDirty = true;
        } else if (ev.handle == gatt.controlPoint && ev.len >= 1) {
            suspended = ev.data[0] == 0; // 0 = Suspend, 1 = Exit Suspend
        } else if (ev.handle == gatt.kbOutput || ev.handle == gatt.bootKbOut) {
            ++stats.outputReports;
            const int kb = e.bootDevice(BootRole::BootKeyboard);
            if (kb >= 0) {
                e.device(kb).onOutput(ev.data, ev.len);
            }
        }
    }

    // Lock held.
    void drainEvents(uint32_t now, bool* advRestart) {
        HidEngine& e = owner->engine();
        const bool lost = overflow;
        for (size_t k = 0; k < eventCount; ++k) {
            const WorkerEvent& ev = events[k];
            switch (ev.type) {
            case Ev::Connected:
                ++stats.connections;
                e.setReady(false);
                e.setBootProtocol(false);
                protocolMode[0] = 1;
                suspended = false;
                subsDirty = true;
                break;
            case Ev::Disconnected:
                ++stats.disconnects;
                stats.lastDisconnectReason = ev.code;
                e.setReady(false);
                e.dropQueued(now);
                for (size_t i = 0; i < e.deviceCount(); ++i) {
                    e.setSubscribed(i, false);
                }
                *advRestart = true;
                break;
            case Ev::Secured:
                if (ev.slot >= 0 && !ev.flag) {
                    ++stats.bondedReconnects;
                }
                if (ev.flag) {
                    pairingOpen = pairingForever = false; // the window closes once a host bonds
                    advDirty = true;
                }
                subsDirty = true;
                break;
            case Ev::SecurityFailed:
            case Ev::PairingRejected:
                break;
            case Ev::Write:
                handleWrite(ev);
                break;
            case Ev::CccdChanged:
                subsDirty = true;
                break;
            case Ev::ServiceChanged:
                ++stats.serviceChanged;
                break;
            }
        }
        eventCount = 0;
        if (lost) {
            // Writes may have been dropped; the SoftDevice wrote the values in place (VLOC_USER).
            overflow = false;
            ++stats.workerOverflows;
            subsDirty = true;
            if (link.conn != kInvalidConn) {
                e.setBootProtocol(gatt.protocolMode && protocolMode[0] == 0);
                const int kb = e.bootDevice(BootRole::BootKeyboard);
                if (kb >= 0) {
                    e.device(kb).onOutput(e.bootProtocol() ? bootKbOut : kbOutBuf, 1);
                }
            }
        }
    }

    bool writeBondFile(size_t slot, const Bond& b) {
        char path[32], tmp[36];
        snprintf(path, sizeof(path), "%s/bond%u", kBondDir, (unsigned)slot);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        uint8_t rec[kBondRecordSize];
        encodeBond(b, rec);
        const int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY);
        if (fd < 0) {
            return false;
        }
        const bool ok = write(fd, rec, sizeof(rec)) == (int)sizeof(rec);
        close(fd);
        // LittleFS rename is atomic: a power cut leaves the old or the new record, never half of one.
        return ok && rename(tmp, path) == 0;
    }

    void persist(uint32_t now) {
        uint8_t dirty, del;
        {
            platform::Guard g;
            dirty = dirtyBonds;
            del = deleteBonds;
        }
        if (!(dirty | del) || now - lastStoreAttempt < kStoreRetryMs) {
            return;
        }
        lastStoreAttempt = now;
        for (size_t i = 0; i < BondTable::kMaxBonds; ++i) {
            const uint8_t bit = 1 << i;
            if (del & bit) {
                char path[32];
                snprintf(path, sizeof(path), "%s/bond%u", kBondDir, (unsigned)i);
                unlink(path);
                platform::Guard g;
                deleteBonds &= ~bit;
            } else if (dirty & bit) {
                Bond copy;
                {
                    platform::Guard g;
                    copy = bonds.slot(i);
                    dirtyBonds &= ~bit;
                }
                if (copy.used && !writeBondFile(i, copy)) {
                    platform::Guard g;
                    dirtyBonds |= bit; // retry
                    ++stats.storeErrors;
                    hidLog.error("bond %u: write failed", (unsigned)i);
                }
            }
        }
    }

    void loadBonds() {
        mkdir("/usr", 0777);
        mkdir(kBondDir, 0777);
        for (size_t i = 0; i < BondTable::kMaxBonds; ++i) {
            char path[32];
            snprintf(path, sizeof(path), "%s/bond%u", kBondDir, (unsigned)i);
            const int fd = open(path, O_RDONLY);
            if (fd < 0) {
                continue;
            }
            uint8_t rec[kBondRecordSize + 1];
            const int n = read(fd, rec, sizeof(rec));
            close(fd);
            Bond b;
            if (n == (int)kBondRecordSize && decodeBond(rec, n, &b)) {
                if (i < bonds.capacity()) {
                    bonds.slot(i) = b;
                } // else kept on flash, inactive, until maxBonds grows again
            } else {
                unlink(path); // a corrupt record must never block startup
                ++stats.storeErrors;
                hidLog.warn("bond %u: discarded unreadable record", (unsigned)i);
            }
        }
    }

    size_t advData(uint8_t* out, bool discoverable) {
        size_t n = 0;
        out[n++] = 2;
        out[n++] = BLE_GAP_AD_TYPE_FLAGS;
        out[n++] = discoverable ? BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE : BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED;
        const uint16_t app = appearance();
        out[n++] = 3;
        out[n++] = BLE_GAP_AD_TYPE_APPEARANCE;
        out[n++] = (uint8_t)app;
        out[n++] = (uint8_t)(app >> 8);
        const bool bat = cfg.battery != BatterySource::None;
        out[n++] = bat ? 5 : 3;
        out[n++] = BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE;
        out[n++] = 0x12;
        out[n++] = 0x18;
        if (bat) {
            out[n++] = 0x0F;
            out[n++] = 0x18;
        }
        const size_t room = BLE_GAP_ADV_SET_DATA_SIZE_MAX - n - 2;
        const size_t len = strlen(cfg.name);
        const size_t put = len < room ? len : room;
        out[n++] = put + 1;
        out[n++] = put == len ? BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME : BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME;
        memcpy(out + n, cfg.name, put);
        return n + put;
    }

    size_t scanResponse(uint8_t* out) {
        const size_t len = strlen(cfg.name);
        out[0] = len + 1;
        out[1] = BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME;
        memcpy(out + 2, cfg.name, len);
        return len + 2;
    }

    uint16_t appearance() {
        if (cfg.appearance) {
            return cfg.appearance;
        }
        HidEngine& e = owner->engine();
        return e.deviceCount() ? e.device(0).appearance() : 0x03C0;
    }

    void applyAdvertising(bool discoverable) {
        uint8_t adv[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
        uint8_t sr[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
        const size_t a = advData(adv, discoverable);
        const size_t s = scanResponse(sr);
        hal_ble_gap_set_advertising_data(adv, a, nullptr);
        hal_ble_gap_set_scan_response_data(sr, s, nullptr);
        advDiscoverable = discoverable;
    }

    void setAdvInterval(int phase) {
        // Apple's accessory guidelines: 20 ms for 30 s, then 152.5 ms, then 1022.5 ms.
        static const uint16_t kIntervals[3] = {32, 244, 1636};
        hal_ble_adv_params_t p = {};
        p.version = BLE_API_VERSION;
        p.size = sizeof(p);
        hal_ble_gap_get_advertising_parameters(&p, nullptr);
        p.interval = kIntervals[phase];
        p.timeout = 0;
        p.type = BLE_ADV_CONNECTABLE_SCANNABLE_UNDIRECRED_EVT;
        p.filter_policy = BLE_ADV_FP_ANY;
        hal_ble_gap_set_advertising_parameters(&p, nullptr);
        advPhase = phase;
    }

    void manageAdvertising(uint32_t now, bool connected, bool restart, bool pairing) {
        if (connected) {
            return;
        }
        if (Network.listening()) {
            advDirty = true; // provisioning owns advertising; take it back afterwards
            advPhase = -1;
            return;
        }
        if (restart) {
            advStartedAt = now;
        }
        if (advDirty || pairing != advDiscoverable) {
            applyAdvertising(pairing);
            advDirty = false;
        }
        const uint32_t age = now - advStartedAt;
        const int phase = age < 30000 ? 0 : age < 300000 ? 1 : 2;
        if (phase != advPhase) {
            setAdvInterval(phase);
        }
        if (now - lastAdvCheck >= kAdvCheckMs) {
            lastAdvCheck = now;
            // Someone else (listening mode, user code) may have replaced our data or stopped advertising.
            uint8_t want[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
            uint8_t have[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
            const size_t w = advData(want, advDiscoverable);
            const ssize_t h = hal_ble_gap_get_advertising_data(have, sizeof(have), nullptr);
            if (h != (ssize_t)w || memcmp(want, have, w) != 0) {
                applyAdvertising(advDiscoverable);
            }
            if (!hal_ble_gap_is_advertising(nullptr)) {
                hal_ble_gap_start_advertising(nullptr);
            }
        }
    }

    void updateBattery(uint32_t now) {
        if (cfg.battery == BatterySource::None) {
            return;
        }
        int level = -1;
        {
            platform::Guard g;
            if (batteryRequested != 0xFF) {
                level = batteryRequested;
                batteryRequested = 0xFF;
            }
        }
        if (level < 0 && cfg.battery == BatterySource::Auto && now - lastBattery >= kBatteryPollMs) {
            lastBattery = now;
            const float c = System.batteryCharge();
            level = c < 0 ? 100 : c > 100 ? 100 : (int)(c + 0.5f);
        }
        if (level < 0 || level == batteryBuf[0]) {
            return;
        }
        batteryBuf[0] = (uint8_t)level;
        platform::Guard g;
        if (link.conn != kInvalidConn && cccdEnabled(link.conn, gatt.batteryCccd)) {
            uint16_t n = 1;
            ble_gatts_hvx_params_t p = {};
            p.handle = gatt.battery;
            p.type = BLE_GATT_HVX_NOTIFICATION;
            p.p_len = &n;
            p.p_data = batteryBuf;
            sd_ble_gatts_hvx(link.conn, &p);
        }
    }

    void computeStatus(uint32_t now) {
        HidEngine& e = owner->engine();
        HidStatus s;
        s.state = error != HidReason::None ? HidState::Error
                : link.conn == kInvalidConn  ? HidState::Advertising
                : e.ready()                  ? HidState::Ready
                                             : HidState::Connected;
        s.reason = error;
        s.bond = link.encSlot;
        s.security = link.conn == kInvalidConn ? 0 : link.secLevel;
        s.pairing = pairingOpen;
        s.bootProtocol = e.bootProtocol();
        s.bondCount = bonds.count();
        status = s;
        (void)now;
    }

    // Without the PHID ABI the library gets no BLE events; find the peripheral link by polling.
    void pollLinkUnpatched(uint32_t now) {
        if (patched || now - lastLinkPoll < 250) {
            return;
        }
        lastLinkPoll = now;
        hal_ble_conn_info_t info = {};
        info.version = BLE_API_VERSION;
        info.size = sizeof(info);
        uint16_t found = kInvalidConn;
        for (uint16_t h = 0; h < 8 && found == kInvalidConn; ++h) {
            if (hal_ble_gap_get_connection_info(h, &info, nullptr) == 0 && info.role == BLE_ROLE_PERIPHERAL) {
                found = h;
            }
        }
        platform::Guard g;
        if (found != link.conn && link.conn != kInvalidConn) {
            post(WorkerEvent{Ev::Disconnected, 0, -1, 0, 0, 0, {}});
            link = Link();
        }
        if (found != kInvalidConn && link.conn == kInvalidConn) {
            link.conn = found;
            link.peerType = info.address.addr_type;
            memcpy(link.peerAddr, info.address.addr, 6);
            link.connectedAt = now;
            post(WorkerEvent{Ev::Connected, 0, -1, 0, 0, 0, {}});
        }
    }

    void work() {
        while (!stopping) {
            os_semaphore_take(wake, 20, false);
            if (stopping) {
                break;
            }
            const uint32_t now = platform::millis();
            pollLinkUnpatched(now);
            bool advRestart = false;
            bool connected, pairingNow;
            {
                platform::Guard g;
                drainEvents(now, &advRestart);
                if (pairingOpen && !pairingForever && (int32_t)(now - pairingUntil) >= 0) {
                    pairingOpen = false;
                }
                if (cfg.autoPair && bonds.count() == 0 && !pairingOpen) {
                    pairingOpen = pairingForever = true;
                }
                connected = link.conn != kInvalidConn;
                pairingNow = pairingOpen;
                // sd_* calls happen under the lock so a reused connection handle can't be hit by mistake.
                // Never hal_ble_* here: the delegate takes this lock on the thread those calls wait for.
                if (disconnectRequested) {
                    disconnectRequested = false;
                    if (connected) {
                        sd_ble_gap_disconnect(link.conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
                    }
                }
                // Ask a host we know (or any host while pairing) to secure the link if it hasn't.
                if (connected && patched && link.secLevel < 2 && !link.pairing && !link.secReqSent &&
                        now - link.connectedAt > kSecurityRequestDelayMs && (link.slot >= 0 || pairingOpen)) {
                    link.secReqSent = true;
                    ble_gap_sec_params_t p = {};
                    p.bond = 1;
                    p.lesc = 1;
                    p.io_caps = BLE_GAP_IO_CAPS_NONE;
                    p.min_key_size = 7;
                    p.max_key_size = 16;
                    sd_ble_gap_authenticate(link.conn, &p);
                }
                if (connected && link.scPending && !link.scSent && (int32_t)(now - link.scRetryAt) >= 0) {
                    sendServiceChanged();
                }

                HidEngine& e = owner->engine();
                if (connected && (subsDirty || now - lastSubsPoll >= kSubscriptionPollMs)) {
                    pollSubscriptions();
                    subsDirty = false;
                    lastSubsPoll = now;
                }
                bool any = false;
                for (size_t i = 0; i < e.deviceCount(); ++i) {
                    any |= e.subscribed(i);
                }
                const bool linkSecure = !secure || link.secLevel >= 2;
                const bool ready = connected && linkSecure && any;
                if (ready && !e.ready()) {
                    stats.lastTimeToReadyMs = now - link.connectedAt;
                }
                e.setReady(ready);
                e.service(now, transport);
                computeStatus(now);
            }
            manageAdvertising(now, connected, advRestart, pairingNow);
            persist(now);
            updateBattery(now);
            HidEngine& e = owner->engine();
            for (size_t i = 0; i < e.deviceCount(); ++i) {
                e.device(i).runCallbacks();
            }
            HidStatus s;
            {
                platform::Guard g;
                s = status;
            }
            if (statusCallback && memcmp(&s, &lastReported, sizeof(s)) != 0) {
                lastReported = s;
                statusCallback(s);
            }
        }
        exited = true;
    }

    static os_thread_return_t workerMain(void* arg) {
        static_cast<BleHidImpl*>(arg)->work();
        os_thread_exit(nullptr);
    }

    int buildGatt() {
        HidEngine& e = owner->engine();
        const bool sec = secure;
        GattHandles h = {};
        ble_gatts_char_handles_t ch = {};
        uint16_t svc;
        uint32_t r;
        hal_ble_lock(nullptr);
        SCOPE_GUARD({ hal_ble_unlock(nullptr); });
#define PHID_TRY(x)                                             \
    do {                                                        \
        if ((r = (x)) != NRF_SUCCESS) {                         \
            hidLog.error("GATT: %s failed: 0x%lx", #x, (unsigned long)r); \
            return SYSTEM_ERROR_NO_MEMORY;                      \
        }                                                       \
    } while (0)
        // Device Information: readable before pairing so hosts can identify us.
        PHID_TRY(addService(0x180A, &svc));
        h.first = svc;
        PHID_TRY(addChar(svc, 0x2A29, R, (uint8_t*)manufacturer, strlen(manufacturer), strlen(manufacturer), false, &ch));
        PHID_TRY(addChar(svc, 0x2A24, R, (uint8_t*)model, strlen(model), strlen(model), false, &ch));
        PHID_TRY(addChar(svc, 0x2A50, R, pnp, 7, 7, false, &ch));
        if (cfg.battery != BatterySource::None) {
            PHID_TRY(addService(0x180F, &svc));
            PHID_TRY(addChar(svc, 0x2A19, R | N, batteryBuf, 1, 1, sec, &ch));
            h.battery = ch.value_handle;
            h.batteryCccd = ch.cccd_handle;
        }
        PHID_TRY(addService(0x1812, &svc));
        const bool hasBoot = e.bootDevice(BootRole::BootKeyboard) >= 0 || e.bootDevice(BootRole::BootMouse) >= 0;
        if (hasBoot) {
            PHID_TRY(addChar(svc, 0x2A4E, R | WNR, protocolMode, 1, 1, sec, &ch));
            h.protocolMode = ch.value_handle;
        }
        for (size_t i = 0; i < e.deviceCount(); ++i) {
            HidDevice& d = e.device(i);
            PHID_TRY(addChar(svc, 0x2A4D, R | N, inputBuf[i], d.inputLength(), d.inputLength(), sec, &ch));
            h.input[i] = ch.value_handle;
            h.inputCccd[i] = ch.cccd_handle;
            PHID_TRY(addReportRef(d.reportId(), 1, sec));
            if (d.outputLength()) {
                PHID_TRY(addChar(svc, 0x2A4D, R | W | WNR, kbOutBuf, 1, 1, sec, &ch));
                h.kbOutput = ch.value_handle;
                PHID_TRY(addReportRef(d.reportId(), 2, sec));
            }
        }
        PHID_TRY(addChar(svc, 0x2A4B, R, const_cast<uint8_t*>(e.reportMap()), e.reportMapLength(), e.reportMapLength(), sec, &ch));
        h.reportMap = ch.value_handle;
        if (e.bootDevice(BootRole::BootKeyboard) >= 0) {
            PHID_TRY(addChar(svc, 0x2A22, R | N, bootKbIn, 8, 8, sec, &ch));
            h.bootKbIn = ch.value_handle;
            h.bootKbInCccd = ch.cccd_handle;
            PHID_TRY(addChar(svc, 0x2A32, R | W | WNR, bootKbOut, 1, 1, sec, &ch));
            h.bootKbOut = ch.value_handle;
        }
        if (e.bootDevice(BootRole::BootMouse) >= 0) {
            PHID_TRY(addChar(svc, 0x2A33, R | N, bootMouseIn, 3, 3, sec, &ch));
            h.bootMouseIn = ch.value_handle;
            h.bootMouseInCccd = ch.cccd_handle;
        }
        PHID_TRY(addChar(svc, 0x2A4A, R, hidInfo, 4, 4, sec, &ch));
        PHID_TRY(addChar(svc, 0x2A4C, WNR, controlPoint, 1, 1, sec, &ch));
        h.controlPoint = ch.value_handle;
        h.last = ch.value_handle;
#undef PHID_TRY
        gatt = h;
        profileBase = crc32(pnp, sizeof(pnp), crc32(e.reportMap(), e.reportMapLength()));
        refreshProfileHash();
        return SYSTEM_ERROR_NONE;
    }

    // Bonds remember which profile the host cached. The hash covers the whole
    // attribute table, so services the app adds with the BLE API (before or after
    // begin()) also trigger Service Changed, not only HID changes.
    void refreshProfileHash() {
        uint32_t hash = profileBase;
        bool afterServiceChanged = false;
        appFirstHandle = 0;
        for (uint32_t handle = 1; handle <= 0xFFFF; ++handle) {
            ble_uuid_t u = {};
            if (sd_ble_gatts_attr_get((uint16_t)handle, &u, nullptr) != NRF_SUCCESS) {
                break;
            }
            // Service Changed may only cover services the app added, which start
            // at the first primary service after the built-in Service Changed characteristic.
            if (u.type == BLE_UUID_TYPE_BLE && u.uuid == BLE_UUID_GATT_CHARACTERISTIC_SERVICE_CHANGED) {
                afterServiceChanged = true;
            } else if (afterServiceChanged && !appFirstHandle && u.type == BLE_UUID_TYPE_BLE && u.uuid == BLE_UUID_SERVICE_PRIMARY) {
                appFirstHandle = (uint16_t)handle;
            }
            uint8_t rec[2 + 16] = {(uint8_t)handle, (uint8_t)(handle >> 8)};
            uint8_t len = 0;
            sd_ble_uuid_encode(&u, &len, rec + 2); // full 128-bit value for vendor UUIDs
            hash = crc32(rec, 2 + len, hash);
        }
        profileHash = hash ? hash : 1;
    }

    void connParams(hal_ble_conn_params_t* p) {
        p->version = BLE_API_VERSION;
        p->size = sizeof(*p);
        switch (cfg.preset) {
        case ConnectionPreset::LowLatency:
            *p = {p->version, p->size, 9, 12, 0, 200};
            break;
        case ConnectionPreset::LowPower:
            *p = {p->version, p->size, 24, 48, 10, 600};
            break;
        default:
            *p = {p->version, p->size, 12, 24, 4, 400};
            break;
        }
    }

    int begin() {
        if (started || (thread && !exited)) {
            return SYSTEM_ERROR_INVALID_STATE; // running, or a previous end() is still stopping
        }
        HidEngine& e = owner->engine();
        constexpr uint32_t kRequiredCaps = PHID_CAP_SEC_DELEGATE | PHID_CAP_SYS_ATTR_DELEGATE | PHID_CAP_SC_CONFIRM;
        phid_info_t info = {};
        info.version = PHID_ABI_VERSION;
        info.size = sizeof(info);
        patched = hal_ble_internal(PHID_OP_GET_INFO, &info, sizeof(info), nullptr) == 0 && info.abi_version >= 1 &&
                (info.caps & kRequiredCaps) == kRequiredCaps;
        if (patched) {
            detail::patchedSystemPartRunning();
        } else if (cfg.installPatch && detail::installPatchedSystemPart() > 0) {
            error = HidReason::InstallingPatch;
            return SYSTEM_ERROR_NOT_SUPPORTED;
        }
        if (!patched && !cfg.allowUnbonded) {
            error = HidReason::NeedsPatchedDeviceOS;
            hidLog.error("This system part has no PHID ABI. Flash the patched system-part1 (see device-os/README.md) "
                         "or call BleHid.allowUnbonded(true) for development.");
            return SYSTEM_ERROR_NOT_SUPPORTED;
        }
        secure = patched;
        if (!gattBuilt) {
            owner->profileError_ = e.buildProfile();
            if (owner->profileError_ != ProfileError::None) {
                error = HidReason::ProfileInvalid;
                hidLog.error("Invalid HID profile: %d", (int)owner->profileError_);
                return SYSTEM_ERROR_INVALID_ARGUMENT;
            }
        }
        e.readyDeadline(cfg.readyDeadline);
        if (!hal_ble_is_initialized(nullptr) && hal_ble_stack_init(nullptr) != 0) {
            error = HidReason::BleStackFailed;
            return SYSTEM_ERROR_INTERNAL;
        }
        if (!wake && os_semaphore_create(&wake, 1, 0) != 0) {
            return SYSTEM_ERROR_NO_MEMORY;
        }
        detail::wakeSemaphore = wake;
        bonds.setCapacity(cfg.maxBonds);
        if (patched) {
            loadBonds();
        }
        if (!gattBuilt) {
            memcpy(pnp, cfg.pnp, sizeof(pnp));
            strncpy(manufacturer, cfg.manufacturer, sizeof(manufacturer) - 1);
            strncpy(model, cfg.model, sizeof(model) - 1);
            if (buildGatt() != SYSTEM_ERROR_NONE) {
                // The SoftDevice can't remove services; a retry would duplicate them.
                error = HidReason::GattFailed;
                return SYSTEM_ERROR_NO_MEMORY;
            }
            gattBuilt = true;
        }
        hal_ble_gap_set_device_name(cfg.name, strlen(cfg.name), nullptr);
        hal_ble_gap_set_appearance((ble_sig_appearance_t)appearance(), nullptr);
        hal_ble_conn_params_t cp = {};
        connParams(&cp);
        hal_ble_gap_set_ppcp(&cp, nullptr);
        hal_ble_pairing_config_t pc = {};
        pc.version = BLE_API_VERSION;
        pc.size = sizeof(pc);
        pc.io_caps = BLE_IO_CAPS_NONE;
        pc.algorithm = BLE_PAIRING_ALGORITHM_AUTO;
        hal_ble_gap_set_pairing_config(&pc, nullptr);
        if (patched) {
            phid_delegate_t d = {};
            d.version = PHID_ABI_VERSION;
            d.size = sizeof(d);
            d.callback = &BleHidImpl::delegate;
            d.context = this;
            d.wake_semaphore = wake;
            hal_ble_internal(PHID_OP_SET_DELEGATE, &d, sizeof(d), nullptr);
        }
        {
            platform::Guard g;
            link = Link();
            eventCount = 0;
            error = HidReason::None;
            pairingOpen = pairingForever = cfg.autoPair && bonds.count() == 0;
        }
        transport.impl = this;
        stopping = false;
        exited = false;
        advDirty = true;
        advPhase = -1;
        advStartedAt = platform::millis();
        hal_ble_gap_set_auto_advertise(BLE_AUTO_ADV_ALWAYS, nullptr);
        applyAdvertising(pairingOpen);
        setAdvInterval(0);
        hal_ble_gap_start_advertising(nullptr);
        if (os_thread_create(&thread, "hid", OS_THREAD_PRIORITY_DEFAULT + 1, &BleHidImpl::workerMain, this, 3072) != 0) {
            return SYSTEM_ERROR_NO_MEMORY;
        }
        started = true;
        hidLog.info("started: %u bond(s), %s, profile 0x%08lx", (unsigned)bonds.count(), patched ? "bonding" : "UNBONDED dev mode",
                (unsigned long)profileHash);
        return SYSTEM_ERROR_NONE;
    }

    void end() {
        if (!started) {
            return;
        }
        stopping = true;
        os_semaphore_give(wake, false);
        for (int i = 0; i < 200 && !exited; ++i) {
            HAL_Delay_Milliseconds(5);
        }
        if (patched) {
            phid_delegate_t d = {};
            d.version = PHID_ABI_VERSION;
            d.size = sizeof(d);
            hal_ble_internal(PHID_OP_SET_DELEGATE, &d, sizeof(d), nullptr);
        }
        hal_ble_gap_set_auto_advertise(BLE_AUTO_ADV_FORBIDDEN, nullptr);
        hal_ble_gap_stop_advertising(nullptr);
        {
            platform::Guard g;
            if (link.conn != kInvalidConn) {
                if (link.encSlot >= 0 && link.secLevel >= 2) {
                    captureSysAttrs(bonds.slot(link.encSlot), link.conn);
                }
                sd_ble_gap_disconnect(link.conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            }
            link = Link();
            owner->engine().setReady(false);
            status = HidStatus();
        }
        persist(platform::millis() + kStoreRetryMs);
        started = false;
    }
};

namespace {
BleHidImpl g_impl;
} // namespace

BleHidClass BleHid;

BleHidClass& BleHidClass::name(const char* n) {
    strncpy(g_impl.cfg.name, n, sizeof(g_impl.cfg.name) - 1);
    return *this;
}
BleHidClass& BleHidClass::manufacturer(const char* n) {
    strncpy(g_impl.cfg.manufacturer, n, sizeof(g_impl.cfg.manufacturer) - 1);
    return *this;
}
BleHidClass& BleHidClass::model(const char* n) {
    strncpy(g_impl.cfg.model, n, sizeof(g_impl.cfg.model) - 1);
    return *this;
}
BleHidClass& BleHidClass::appearance(uint16_t a) {
    g_impl.cfg.appearance = a;
    return *this;
}
BleHidClass& BleHidClass::pnpId(uint8_t source, uint16_t vid, uint16_t pid, uint16_t version) {
    uint8_t* p = g_impl.cfg.pnp;
    p[0] = source;
    p[1] = (uint8_t)vid;
    p[2] = (uint8_t)(vid >> 8);
    p[3] = (uint8_t)pid;
    p[4] = (uint8_t)(pid >> 8);
    p[5] = (uint8_t)version;
    p[6] = (uint8_t)(version >> 8);
    return *this;
}
BleHidClass& BleHidClass::add(HidDevice& d) {
    engine_.add(d);
    return *this;
}
BleHidClass& BleHidClass::connection(ConnectionPreset p) {
    g_impl.cfg.preset = p;
    return *this;
}
BleHidClass& BleHidClass::maxBonds(uint8_t n) {
    g_impl.cfg.maxBonds = n < 1 ? 1 : n > BondTable::kMaxBonds ? BondTable::kMaxBonds : n;
    return *this;
}
BleHidClass& BleHidClass::evictOldestBond(bool e) {
    g_impl.cfg.evict = e;
    return *this;
}
BleHidClass& BleHidClass::autoPairWhenEmpty(bool on) {
    g_impl.cfg.autoPair = on;
    return *this;
}
BleHidClass& BleHidClass::battery(BatterySource s) {
    g_impl.cfg.battery = s;
    return *this;
}
BleHidClass& BleHidClass::readyDeadline(uint32_t ms) {
    g_impl.cfg.readyDeadline = ms;
    return *this;
}
BleHidClass& BleHidClass::installPatch(bool on) {
    g_impl.cfg.installPatch = on;
    return *this;
}
BleHidClass& BleHidClass::allowUnbonded(bool on) {
    g_impl.cfg.allowUnbonded = on;
    return *this;
}

int BleHidClass::begin() {
    return g_impl.begin();
}

void BleHidClass::end() {
    g_impl.end();
}

void BleHidClass::startPairing(uint32_t durationMs) {
    platform::Guard g;
    g_impl.pairingOpen = true;
    g_impl.pairingForever = false;
    g_impl.pairingUntil = platform::millis() + durationMs;
    platform::wake();
}

void BleHidClass::stopPairing() {
    platform::Guard g;
    g_impl.pairingOpen = false;
    g_impl.pairingForever = false;
    platform::wake();
}

bool BleHidClass::pairing() const {
    platform::Guard g;
    return g_impl.pairingOpen;
}

size_t BleHidClass::bondCount() const {
    platform::Guard g;
    return g_impl.bonds.count();
}

bool BleHidClass::forget(size_t slot) {
    platform::Guard g;
    if (slot >= g_impl.bonds.capacity() || !g_impl.bonds.slot(slot).used) {
        return false;
    }
    g_impl.bonds.clear(slot);
    g_impl.deleteBonds |= 1 << slot;
    g_impl.dirtyBonds &= ~(1 << slot);
    if (g_impl.link.slot == (int)slot || g_impl.link.encSlot == (int)slot) {
        g_impl.disconnectRequested = true;
    }
    platform::wake();
    return true;
}

void BleHidClass::forgetAll() {
    for (size_t i = 0; i < BondTable::kMaxBonds; ++i) {
        forget(i);
    }
}

void BleHidClass::batteryLevel(uint8_t percent) {
    platform::Guard g;
    g_impl.batteryRequested = percent > 100 ? 100 : percent;
    platform::wake();
}

HidStatus BleHidClass::status() const {
    platform::Guard g;
    HidStatus s = g_impl.status;
    if (!g_impl.started) {
        s.state = g_impl.error != HidReason::None ? HidState::Error : HidState::Off;
        s.reason = g_impl.error;
    }
    return s;
}

bool BleHidClass::ready() const {
    return engine_.ready();
}

bool BleHidClass::waitReady(uint32_t timeoutMs) {
    const uint32_t start = platform::millis();
    while (!ready()) {
        if (platform::millis() - start >= timeoutMs) {
            return false;
        }
        platform::sleepMs(10);
    }
    return true;
}

bool BleHidClass::deviceOsSupported() {
    phid_info_t info = {};
    info.version = PHID_ABI_VERSION;
    info.size = sizeof(info);
    return hal_ble_internal(PHID_OP_GET_INFO, &info, sizeof(info), nullptr) == 0 && info.abi_version >= 1;
}

void BleHidClass::onStatus(void (*callback)(const HidStatus&)) {
    g_impl.statusCallback = callback;
}

BleHidStats BleHidClass::stats() const {
    platform::Guard g;
    BleHidStats s = g_impl.stats;
    s.reports = engine_.stats();
    return s;
}

} // namespace phid

#else // not an nRF52840 Particle device

namespace phid {

BleHidClass BleHid;

BleHidClass& BleHidClass::name(const char*) { return *this; }
BleHidClass& BleHidClass::manufacturer(const char*) { return *this; }
BleHidClass& BleHidClass::model(const char*) { return *this; }
BleHidClass& BleHidClass::appearance(uint16_t) { return *this; }
BleHidClass& BleHidClass::pnpId(uint8_t, uint16_t, uint16_t, uint16_t) { return *this; }
BleHidClass& BleHidClass::add(HidDevice& d) { engine_.add(d); return *this; }
BleHidClass& BleHidClass::connection(ConnectionPreset) { return *this; }
BleHidClass& BleHidClass::maxBonds(uint8_t) { return *this; }
BleHidClass& BleHidClass::evictOldestBond(bool) { return *this; }
BleHidClass& BleHidClass::autoPairWhenEmpty(bool) { return *this; }
BleHidClass& BleHidClass::battery(BatterySource) { return *this; }
BleHidClass& BleHidClass::readyDeadline(uint32_t) { return *this; }
BleHidClass& BleHidClass::installPatch(bool) { return *this; }
BleHidClass& BleHidClass::allowUnbonded(bool) { return *this; }
int BleHidClass::begin() { return -120; /* SYSTEM_ERROR_NOT_SUPPORTED */ }
void BleHidClass::end() {}
void BleHidClass::startPairing(uint32_t) {}
void BleHidClass::stopPairing() {}
bool BleHidClass::pairing() const { return false; }
size_t BleHidClass::bondCount() const { return 0; }
bool BleHidClass::forget(size_t) { return false; }
void BleHidClass::forgetAll() {}
void BleHidClass::batteryLevel(uint8_t) {}
HidStatus BleHidClass::status() const {
    HidStatus s;
    s.state = HidState::Error;
    s.reason = HidReason::UnsupportedPlatform;
    return s;
}
bool BleHidClass::ready() const { return false; }
bool BleHidClass::waitReady(uint32_t) { return false; }
bool BleHidClass::deviceOsSupported() { return false; }
void BleHidClass::onStatus(void (*)(const HidStatus&)) {}
BleHidStats BleHidClass::stats() const { return BleHidStats(); }

} // namespace phid

#endif
