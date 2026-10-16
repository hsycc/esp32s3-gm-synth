#pragma once

#include "voice/VoiceRenderer.h"

#include <cstdint>

namespace gm_synth::sf2 {

constexpr std::int32_t kSf2DefaultVelocityFilterCutoffCents = -2400;
constexpr std::int32_t kSf2MinFilterCutoffCents = 1500;
constexpr std::int32_t kSf2MaxFilterCutoffCents = 13500;
constexpr std::uint32_t kSf2FilterOutputSampleRate = 44100;

// SoundFont 2.04 default velocity->initialFilterFc route: negative-unipolar
// linear Note-On velocity, no secondary source, amount -2400 cents. The
// endpoints follow the SF2 controller domain (1 -> 127/128, 127 -> 0).
std::int32_t velocityToFilterCutoffCents(
    std::uint8_t velocity,
    std::int32_t amount_cents = kSf2DefaultVelocityFilterCutoffCents);

// Builds the 12 dB/octave low-pass used by the current embedded profile. The
// coefficient path is integer/table based and is safe to execute at NoteOn.
// For now only 44.1 kHz and zero-cB resonance are qualified; callers must keep
// non-zero initialFilterQ explicit as pending rather than silently approximating
// it. Coefficients follow the transposed-direct-form-II topology used by
// TinySoundFont's reference low-pass implementation.
bool prepareLowpassQ24(
    std::int32_t cutoff_cents,
    std::int32_t resonance_cb,
    std::uint32_t output_sample_rate,
    voice::BiquadLowpassConfig* output);

}  // namespace gm_synth::sf2
