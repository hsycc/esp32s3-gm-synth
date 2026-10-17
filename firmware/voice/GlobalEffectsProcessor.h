#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {

// Fixed-cost global effects stage for the SoundFont chorus/reverb send buses.
// The algorithms are intentionally compact rather than model-specific: SF2
// defines send levels but does not prescribe a particular chorus/reverb DSP.
// All delay storage is inline, fixed-size, and allocation-free.
class GlobalEffectsProcessor {
public:
    static constexpr std::uint32_t kSampleRate = 44100;

    void reset();

    // Adds wet chorus/reverb output into interleaved_stereo. The input buses are
    // already post-voice gain/pan/envelope and scaled by the per-voice SF2 send
    // amount. Passing null for either bus is equivalent to a silent send while
    // still allowing that effect's existing tail to decay.
    void processAdd(
        const std::int32_t* chorus_bus,
        const std::int32_t* reverb_bus,
        std::int32_t* interleaved_stereo,
        std::size_t frame_count);

private:
    static std::int16_t clampI16(std::int32_t value);
    static std::int32_t scaleQ15(std::int32_t value, std::int32_t gain_q15);
    static std::int32_t averageStereo(const std::int32_t* bus, std::size_t frame);
    static std::int32_t triangleQ15(std::uint32_t phase_q32);

    std::int32_t processComb(
        std::int32_t input,
        std::int16_t* buffer,
        std::size_t size,
        std::size_t* index,
        std::int32_t feedback_q15);
    std::int32_t processAllpass(
        std::int32_t input,
        std::int16_t* buffer,
        std::size_t size,
        std::size_t* index);
    std::int32_t chorusTap(std::size_t delay_frames) const;

    // 1024 frames is enough for a ~11..16 ms modulated chorus delay at 44.1 kHz.
    std::array<std::int16_t, 1024> chorus_delay_{};
    std::size_t chorus_write_ = 0;
    std::uint32_t chorus_phase_q32_ = 0;

    // Three short mutually-prime-ish combs plus two all-pass stages form a
    // low-memory Schroeder-style late tail. Total delay storage remains below
    // 8 KiB so the processor is suitable for the S3 target without PSRAM reads
    // in the audio hot path.
    std::array<std::int16_t, 601> reverb_comb_a_{};
    std::array<std::int16_t, 787> reverb_comb_b_{};
    std::array<std::int16_t, 997> reverb_comb_c_{};
    std::array<std::int16_t, 173> reverb_allpass_l_{};
    std::array<std::int16_t, 211> reverb_allpass_r_{};
    std::size_t reverb_comb_a_index_ = 0;
    std::size_t reverb_comb_b_index_ = 0;
    std::size_t reverb_comb_c_index_ = 0;
    std::size_t reverb_allpass_l_index_ = 0;
    std::size_t reverb_allpass_r_index_ = 0;
};

}  // namespace gm_synth::voice
