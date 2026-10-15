#pragma once

#include "VoiceRenderer.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {

struct AudioBlockStats {
    std::uint64_t blocks_rendered = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t clipped_samples = 0;
    std::int32_t peak_abs_accumulator = 0;
};

// Final software boundary immediately before the hardware I2S sink.
//
// The block renderer owns its fixed int32 mix scratch and therefore performs no
// allocation and consumes no multi-kilobyte task-stack scratch. M5 will inject
// timestamped events at sub-block offsets; M4 first establishes a deterministic
// fixed 64-frame render/output contract.
class AudioBlockRenderer {
public:
    static constexpr std::uint32_t kOutputSampleRate = 44100;
    static constexpr std::size_t kFramesPerBlock = 64;
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kSamplesPerBlock = kFramesPerBlock * kChannels;

    explicit AudioBlockRenderer(VoicePool& voices)
        : voices_(voices) {}

    // Renders exactly one 64-frame stereo block into signed 16-bit interleaved
    // output. The caller owns the I2S write and realtime deadline measurement.
    void render(std::int16_t* interleaved_stereo);

    const AudioBlockStats& stats() const { return stats_; }

private:
    static std::int16_t saturateToI16(std::int32_t sample, bool* clipped);

    VoicePool& voices_;
    std::array<std::int32_t, kSamplesPerBlock> mix_{};
    AudioBlockStats stats_{};
};

}  // namespace gm_synth::voice
