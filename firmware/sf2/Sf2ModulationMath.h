#pragma once

#include <cstdint>

namespace gm_synth::sf2 {

// SoundFont default attenuation modulators for Note-On velocity, CC7 and CC11
// are negative-unipolar concave sources with a 960 cB amount. These helpers use
// fixed-point/integer arithmetic only so realtime event application never calls
// log()/pow().
constexpr std::int32_t kSf2DefaultVelocityAttenuationCb = 960;
constexpr std::int32_t kSf2DefaultControllerAttenuationCb = 960;
constexpr std::int32_t kSf2DefaultPanAmount = 1000;
constexpr std::int32_t kSf2MaxAttenuationCb = 1440;

// Generic negative-unipolar concave source transform for a 7-bit MIDI value.
// value=127 contributes zero; lower values contribute progressively more of
// amount. Signed amounts are supported for file-defined modulator replacement.
std::int32_t negativeConcaveToAmount(
    std::uint8_t value,
    std::int32_t amount);

std::int32_t velocityToAttenuationCb(
    std::uint8_t velocity,
    std::int32_t amount_cb);

std::int32_t controllerToAttenuationCb(
    std::uint8_t value,
    std::int32_t amount_cb = kSf2DefaultControllerAttenuationCb);

// SoundFont default CC10 route: positive-bipolar linear source, amount 1000
// tenths of a percent. The returned contribution is added to the zone pan
// generator before the final [-500, 500] generator clamp.
std::int32_t controllerToPanSf2Units(
    std::uint8_t value,
    std::int32_t amount = kSf2DefaultPanAmount);

// Converts a final initialAttenuation value to Q15 amplitude. SoundFont clamps
// the final attenuation summing node to [0, 1440] cB; negative preset offsets
// can therefore cancel velocity/controller attenuation but can never amplify
// above unity.
std::int32_t attenuationCbToGainQ15(std::int32_t attenuation_cb);

// Current runtime pan law remains the lightweight linear balance used by the
// existing materializer. Keeping it here lets dynamic note-on/controller
// attenuation be combined before pan without losing negative static offsets.
void noteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t pan_sf2_units,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15);

// Same gain path with the mandatory default GM/SF2 CC7, CC10 and CC11 routes
// applied at the final summing nodes. Route amounts are parameters so the
// PlaybackZone representation can later carry file-defined replacements.
void channelNoteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t zone_pan_sf2_units,
    std::uint8_t channel_volume,
    std::int32_t volume_to_attenuation_cb,
    std::uint8_t channel_pan,
    std::int32_t pan_amount,
    std::uint8_t channel_expression,
    std::int32_t expression_to_attenuation_cb,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15);

}  // namespace gm_synth::sf2
