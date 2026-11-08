#pragma once

namespace phid {
namespace detail {

// Installs the patched system part bundled as an Asset OTA asset (a name
// containing "+phid" and ending in ".bin") when it replaces the running system
// part of the same version. Returns 1 when the update was staged and the device
// is about to reset, 0 when there's nothing to install, or a SYSTEM_ERROR_* code.
int installPatchedSystemPart();

// Called once the running system part has the PHID ABI.
void patchedSystemPartRunning();

} // namespace detail
} // namespace phid
