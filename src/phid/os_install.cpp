// system_info_get_unstable() lists the running modules. Its structs can change
// between Device OS releases; this file only acts when the versions match.
#define PARTICLE_USE_UNSTABLE_API

#include "os_install.h"

#if defined(PARTICLE)
#include "Particle.h"
#endif

#if defined(PARTICLE) && HAL_PLATFORM_NRF52840

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <new>

#include "file_transfer.h"
#include "module_info.h"
#include "ota_flash_hal.h"
#include "system_error.h"
#include "system_info.h"
#include "system_update.h"

namespace phid {
namespace detail {

namespace {

Logger osLog("hid");

// Remembers install attempts per asset, so a system part that doesn't take
// can't put the device in a reset loop.
constexpr const char* kMarkerPath = "/usr/phid/os-install";
constexpr uint32_t kMarkerMagic = 0x50484f53; // "PHOS"
constexpr uint8_t kMaxAttempts = 2;
// On Boron and Argon the OTA area runs into the asset filesystem past this size,
// and the local update path doesn't unmount it the way a cloud update does.
constexpr size_t kMaxImage = 606208;
constexpr size_t kChunk = 1024;
// protocol::UpdateFlag values taken by Spark_Finish_Firmware_Update.
constexpr uint32_t kUpdateSuccess = 0x01;
constexpr uint32_t kUpdateValidateOnly = 0x02;

struct Marker {
    uint32_t magic;
    uint8_t hash[8]; // first bytes of the asset's SHA-256
    uint8_t attempts;
    uint8_t reserved[3];
};

int tryInstall();

bool isPatchAsset(const ApplicationAsset& a) {
    const String n = a.name();
    return n.indexOf("+phid") >= 0 && n.endsWith(".bin");
}

// The asset must replace a running system part of the same version: same
// platform, function, index, start address, and version.
bool replacesRunningModule(ApplicationAsset& a) {
    hal_system_info_t info = {};
    info.size = sizeof(info);
    if (system_info_get_unstable(&info, 0, nullptr) != 0) {
        return false;
    }
    bool match = false;
    for (unsigned i = 0; i < info.module_count && !match; ++i) {
        const hal_module_t& m = info.modules[i];
        if (m.bounds.store != MODULE_STORE_MAIN || m.info.module_function != MODULE_FUNCTION_SYSTEM_PART) {
            continue;
        }
        // The module info follows the vector table, at the same offset in the image.
        module_info_t h = {};
        a.reset();
        if (a.skip(m.module_info_offset) < 0 || a.read((char*)&h, sizeof(h)) != (int)sizeof(h)) {
            break;
        }
        match = h.platform_id == PLATFORM_ID && h.platform_id == m.info.platform_id &&
                h.module_function == m.info.module_function && h.module_index == m.info.module_index &&
                h.module_version == m.info.module_version && h.module_start_address == m.info.module_start_address;
    }
    system_info_free_unstable(&info, nullptr);
    return match;
}

bool readMarker(Marker* m) {
    const int fd = open(kMarkerPath, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = read(fd, m, sizeof(*m)) == (int)sizeof(*m) && m->magic == kMarkerMagic;
    close(fd);
    return ok;
}

bool writeMarker(const Marker& m) {
    mkdir("/usr", 0777);
    mkdir("/usr/phid", 0777);
    const int fd = open(kMarkerPath, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = write(fd, &m, sizeof(m)) == (int)sizeof(m);
    return close(fd) == 0 && ok;
}

int stream(ApplicationAsset& a, FileTransfer::Descriptor& d) {
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
    if (!buf) {
        return SYSTEM_ERROR_NO_MEMORY;
    }
    a.reset();
    for (size_t offset = 0; offset < d.file_length;) {
        const size_t want = std::min(kChunk, (size_t)d.file_length - offset);
        const int n = a.read(buf.get(), want);
        if (n != (int)want) {
            return n < 0 ? n : SYSTEM_ERROR_BAD_DATA;
        }
        d.chunk_size = n;
        d.chunk_address = d.file_address + offset;
        const int r = Spark_Save_Firmware_Chunk(d, (const uint8_t*)buf.get(), nullptr);
        if (r != 0) {
            return r;
        }
        offset += n;
    }
    return 0;
}

} // namespace

int installPatchedSystemPart() {
    // At most one try per boot: 1 means staged and waiting for the reset.
    static bool tried = false;
    static int result = 0;
    if (tried) {
        return result;
    }
    tried = true;
    result = tryInstall();
    return result;
}

void patchedSystemPartRunning() {
    Marker m;
    if (readMarker(&m)) {
        osLog.info("Patched system part installed (attempt %u)", (unsigned)m.attempts);
        unlink(kMarkerPath);
    }
}

namespace {

int tryInstall() {
    ApplicationAsset asset;
    bool found = false;
    for (ApplicationAsset& a : System.assetsAvailable()) {
        if (!isPatchAsset(a)) {
            continue;
        }
        if (!replacesRunningModule(a)) {
            osLog.warn("%s doesn't match this device's Device OS; skipping it", a.name().c_str());
            continue;
        }
        asset = a;
        found = true;
        break;
    }
    if (!found) {
        return 0;
    }

    const AssetHash hash = asset.hash(); // keep it alive: hash() returns a reference into it
    const Buffer& sha = hash.hash();
    Marker m = {};
    if (sha.size() < sizeof(m.hash)) {
        return SYSTEM_ERROR_BAD_DATA;
    }
    if (!readMarker(&m) || memcmp(m.hash, sha.data(), sizeof(m.hash)) != 0) {
        m = {};
        m.magic = kMarkerMagic;
        memcpy(m.hash, sha.data(), sizeof(m.hash));
    }
    // Pressing RESET or power-cycling allows one more try; resets caused by the
    // update itself don't, so a patch that never takes can't loop.
    const int reason = System.resetReason();
    if (m.attempts >= kMaxAttempts && (reason == RESET_REASON_PIN_RESET || reason == RESET_REASON_POWER_DOWN)) {
        m.attempts = kMaxAttempts - 1;
    }
    if (m.attempts >= kMaxAttempts) {
        osLog.error("Installing %s failed %u times; not retrying. Flash it by hand (see device-os/README.md).",
                asset.name().c_str(), (unsigned)m.attempts);
        return SYSTEM_ERROR_LIMIT_EXCEEDED;
    }
    if (!System.updatesEnabled() && !System.updatesForced()) {
        return SYSTEM_ERROR_OTA_UPDATES_DISABLED;
    }
    if (asset.size() > kMaxImage) {
        osLog.error("%s is too large to install from the app", asset.name().c_str());
        return SYSTEM_ERROR_TOO_LARGE;
    }
    if (System.updateStatus() == SYSTEM_UPDATE_STATUS_IN_PROGRESS) {
        return SYSTEM_ERROR_BUSY;
    }
    osLog.warn("Installing %s (%u bytes, attempt %u of %u). The device resets when it's done.", asset.name().c_str(),
            (unsigned)asset.size(), (unsigned)m.attempts + 1, (unsigned)kMaxAttempts);
    FileTransfer::Descriptor d;
    d.file_length = asset.size();
    d.chunk_address = 0;
    d.chunk_size = kChunk;
    d.store = FileTransfer::Store::FIRMWARE;
    int r = Spark_Prepare_For_Firmware_Update(d, 0, nullptr);
    if (r != 0) {
        osLog.error("Can't start the update: %d", r); // e.g. Device Protection; doesn't count as an attempt
        return r;
    }
    // Count the attempt before anything can reset the device.
    ++m.attempts;
    if (!writeMarker(m)) {
        Spark_Finish_Firmware_Update(d, 0, nullptr);
        return SYSTEM_ERROR_FILESYSTEM;
    }
    r = stream(asset, d);
    if (r == 0) {
        // Integrity and dependency checks, without applying or resetting.
        r = Spark_Finish_Firmware_Update(d, kUpdateSuccess | kUpdateValidateOnly, nullptr);
    }
    if (r != 0) {
        Spark_Finish_Firmware_Update(d, 0, nullptr); // cancel; doesn't reset
        osLog.error("%s failed validation: %d", asset.name().c_str(), r);
        return r;
    }
    // Applies on the next boot; Device OS resets the device shortly.
    r = Spark_Finish_Firmware_Update(d, kUpdateSuccess, nullptr);
    if (r != 0) {
        osLog.error("Can't apply %s: %d", asset.name().c_str(), r);
        return r;
    }
    return 1;
}

} // namespace

} // namespace detail
} // namespace phid

#endif
