#pragma once

#include <cstdint>

namespace gm_synth::voice {

// Converts an already-resolved pitch offset into the Q32.32 source-frame
// increment consumed by VoiceRenderer. This belongs to the control/start path,
// never the per-frame render loop.
//
// cents_from_sample_root is intentionally SF2-agnostic: M2/M2.5 own the exact
// generator/modulator/root-key/pitch-correction semantics and pass the final
// signed cents here.
std::uint64_t phaseIncrementQ32(
    std::uint32_t source_sample_rate,
    std::uint32_t output_sample_rate,
    double cents_from_sample_root);

}  // namespace gm_synth::voice
