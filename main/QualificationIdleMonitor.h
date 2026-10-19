#pragma once

namespace gm_synth::bringup {

// Registers a diagnostic CPU0 idle hook in the dedicated physical
// qualification build. Other builds intentionally no-op so QEMU/normal
// firmware timing and scheduling remain unchanged.
bool initializeQualificationIdleMonitor();

// Blocks the caller for a bounded multi-tick window and proves that CPU0's
// FreeRTOS idle hook actually ran during that window. A failure is sticky for
// the remainder of the qualification run.
bool verifiedQualificationIdleWindow(const char* phase);

bool qualificationIdleMonitorHealthy();

}  // namespace gm_synth::bringup
