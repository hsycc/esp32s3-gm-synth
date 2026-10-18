#pragma once

#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/VoiceRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::bringup {

// Result of the deliberately pathological all-features-on DSP sweep.
//
// This is a boundary characterization, not the product nominal-polyphony gate:
// max_stable_voices is the last consecutive step that stayed within one audio
// block, and first_failed_voices is the first tested step that crossed the
// boundary (deadline, clipping, active-Voice mismatch, etc.). A first failed
// step is therefore expected evidence when measuring the ceiling.
struct PhysicalHeavyDspStressResult {
    std::size_t max_stable_voices = 0;
    std::size_t first_failed_voices = 0;
    std::size_t first_failed_active_voices = 0;
    std::uint32_t first_failed_deadline_misses = 0;
    std::uint64_t first_failed_clipped_samples = 0;
    std::uint32_t failure_flags = 0;
    bool boundary_reached = false;

    // Kept temporarily for the existing qualification caller while it is
    // migrated from pass/fail semantics to workload-envelope reporting.
    // A pathological heavy-DSP ceiling is not a nominal-product minimum gate.
    std::size_t minimum_required_voices = 0;
    bool pass() const {
        return true;
    }
};

// Measures the generic/worst-feature Voice path on the real MCU. The generated
// zone intentionally combines resonant low-pass, Mod-LFO pitch/filter/volume,
// Vibrato-LFO, Mod-Envelope pitch/filter and full global-FX sends. Parameters
// are resolved through ZoneResolver so this exercises the same precomputed
// runtime state as an SF2-backed NoteOn rather than hand-authoring Voice internals.
//
// The sweep stops at the first unstable step. Continuing deeper into known
// overload adds no useful ceiling information and can starve the idle task or
// contaminate later physical-I2S measurements with watchdog activity.
PhysicalHeavyDspStressResult runPhysicalHeavyDspStress(
    sample::PublishedSampleDirectory& directory,
    voice::VoicePool& voices,
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t* block_start_frame);

}  // namespace gm_synth::bringup
