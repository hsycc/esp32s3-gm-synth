#pragma once

#include <cstdint>

namespace gm_synth::sf2 {

// SoundFont's default Note-On velocity -> initialAttenuation modulator is a
// negative unipolar concave source with a 960 cB default amount. The helpers
// below intentionally use fixed-point/integer arithmetic only so NoteOn
// resolution never calls log()/pow() in the audio domain.
constexpr std::int32_t kSf2DefaultVelocityAttenuationCb = 960;
constexpr std::int32_t kSf2MaxAttenuationCb = 1440;

// Returns the modulator contribution in SoundFont centibels for an effective
// note-on velocity. Signed amounts are supported because file-defined
// instrument/preset modulators may replace/add to the default route.
std::int32_t velocityToAttenuationCb(
    std::uint8_t velocity,
    std::int32_t amount_cb);

// Converts a final initialAttenuation value to Q15 amplitude. SoundFont clamps
// the final attenuation summing node to [0, 1440] cB; negative preset offsets
// can therefore cancel velocity/controller attenuation but can never amplify
// above unity.
std::int32_t attenuationCbToGainQ15(std::int32_t attenuation_cb);

// Current runtime pan law remains the lightweight linear balance used by the
// existing materializer. Keeping it here lets dynamic note-on attenuation be
// combined before pan without losing negative static attenuation offsets.
void noteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t pan_sf2_units,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15);

}  // namespace gm_synth::sf2
