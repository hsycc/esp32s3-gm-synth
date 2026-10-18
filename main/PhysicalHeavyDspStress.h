#pragma once

#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/VoiceRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::bringup {

struct PhysicalHeavyDspStressResult {
    std::size_t max_stable_voices = 0;
    std::size_t minimum_required_voices = 16;

    bool pass() const {
        return max_stable_voices >= minimum_required_voices;
    }
};

// Measures the generic/worst-feature Voice path on the real MCU. The generated
// zone intentionally combines resonant low-pass, Mod-LFO pitch/filter/volume,
// Vibrato-LFO, Mod-Envelope pitch/filter and full global-FX sends. Parameters
// are resolved through ZoneResolver so this exercises the same precomputed
// runtime state as an SF2-backed NoteOn rather than hand-authoring Voice internals.
PhysicalHeavyDspStressResult runPhysicalHeavyDspStress(
    sample::PublishedSampleDirectory& directory,
    voice::VoicePool& voices,
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t* block_start_frame);

}  // namespace gm_synth::bringup
