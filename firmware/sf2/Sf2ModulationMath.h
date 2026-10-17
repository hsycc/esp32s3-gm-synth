#pragma once

#include <cstdint>

namespace gm_synth::sf2 {

// SoundFont default attenuation modulators for Note-On velocity, CC7 and CC11
// are negative-unipolar concave sources with a 960 cB amount. These helpers use
// fixed-point/integer arithmetic only so realtime event application never calls
// log()/pow().
constexpr std::int32_t kSf2DefaultVelocityAttenuationCb = 960;
constexpr std::int32_t kSf2DefaultControllerAttenuationCb = 960;
// CC10 is positive-bipolar linear. With the SF2 128-step controller domain an
// amount of 500 maps CC0 -> -500, CC64 -> 0 and CC127 -> +492, matching the
// default SoundFont/MIDI pan mapping without prematurely saturating the pan
// generator summing node.
constexpr std::int32_t kSf2DefaultPanAmount = 500;
// SF2 default Channel Pressure and CC1 routes each add up to 50 cents to the
// vibrato-LFO-to-pitch destination.
constexpr std::int32_t kSf2DefaultVibratoControllerAmountCents = 50;
// SF2 default CC91/CC93 modulators add up to 200 tenths of a percent to the
// reverb/chorus generator summing node.
constexpr std::int32_t kSf2DefaultEffectsSendAmount = 200;
constexpr std::int32_t kSf2MaxEffectsSend = 1000;
constexpr std::int32_t kSf2MaxAttenuationCb = 1440;

// Generic negative-unipolar concave source transform for a 7-bit MIDI value.
// value=127 contributes zero; lower values contribute progressively more of
// amount. Signed amounts are supported for file-defined modulator replacement.
std::int32_t negativeConcaveToAmount(
    std::uint8_t value,
    std::int32_t amount);

// Positive-unipolar linear SoundFont source. The SF2 controller model uses 128
// discrete steps, so MIDI value 127 contributes 127/128 of amount.
std::int32_t positiveLinearToAmount(
    std::uint8_t value,
    std::int32_t amount);

std::int32_t velocityToAttenuationCb(
    std::uint8_t velocity,
    std::int32_t amount_cb);

std::int32_t controllerToAttenuationCb(
    std::uint8_t value,
    std::int32_t amount_cb = kSf2DefaultControllerAttenuationCb);

// SoundFont default CC10 route: positive-bipolar linear source, amount 500
// pan units. The returned contribution is added to the zone pan generator
// before the final [-500, 500] generator clamp.
std::int32_t controllerToPanSf2Units(
    std::uint8_t value,
    std::int32_t amount = kSf2DefaultPanAmount);

// Positive-unipolar linear SoundFont controller source used by CC91/CC93.
// SF2 maps MIDI 127 to 127/128 of the modulator amount rather than exact unity.
std::int32_t controllerToEffectsSendSf2Units(
    std::uint8_t value,
    std::int32_t amount = kSf2DefaultEffectsSendAmount);

// Applies one prepared static effects-send generator plus its current channel
// controller route and clamps the final generator summing node to [0, 1000].
std::int32_t channelEffectsSendSf2Units(
    std::int32_t static_send_sf2_units,
    std::uint8_t controller_value,
    std::int32_t controller_amount = kSf2DefaultEffectsSendAmount);

// Converts a final initialAttenuation value to Q15 amplitude. SoundFont clamps
// the final attenuation summing node to [0, 1440] cB; negative preset offsets
// can therefore cancel velocity/controller attenuation but can never amplify
// above unity.
std::int32_t attenuationCbToGainQ15(std::int32_t attenuation_cb);

// Final gain uses the SF2 equal-power pan law. Attenuation and pan are combined
// after their respective generator/modulator summing nodes have been resolved.
void noteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t pan_sf2_units,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15);

// Same gain path with the mandatory default GM/SF2 CC7, CC10 and CC11 routes
// applied at the final summing nodes. Route amounts are parameters so the
// PlaybackZone representation can carry file-defined replacements.
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
