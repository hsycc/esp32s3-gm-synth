#pragma once

#include "Sf2ModulationMath.h"
#include "Sf2PdtaReader.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

// Compact modulation state needed by the current runtime. Mandatory realtime
// routes are carried explicitly at their SF2 defaults and file-defined routes
// with the same identity may replace/augment them according to SF2 hierarchy.
struct ResolvedZoneModulation {
    std::int32_t velocity_to_attenuation_cb = kSf2DefaultVelocityAttenuationCb;
    std::int32_t volume_to_attenuation_cb = kSf2DefaultControllerAttenuationCb;
    std::int32_t pan_amount = kSf2DefaultPanAmount;
    std::int32_t expression_to_attenuation_cb = kSf2DefaultControllerAttenuationCb;
    std::int32_t reverb_send_amount = kSf2DefaultEffectsSendAmount;
    std::int32_t chorus_send_amount = kSf2DefaultEffectsSendAmount;
    std::int32_t pitch_wheel_to_pitch_cents = 12700;

    std::uint16_t supported_velocity_attenuation_records = 0;
    std::uint16_t supported_effects_send_records = 0;
    std::uint16_t unsupported_modulator_records = 0;

    bool hasUnsupportedModulators() const {
        return unsupported_modulator_records != 0;
    }
};

enum class ZoneModulatorResolveStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    PresetNotFound,
    ReaderError,
    OutputTooSmall,
};

struct ZoneModulatorResolveResult {
    ZoneModulatorResolveStatus status = ZoneModulatorResolveStatus::InvalidArgument;
    std::size_t zone_count = 0;
    std::size_t required_capacity = 0;
    std::uint16_t preset_index = 0;
    std::uint32_t supported_records = 0;
    std::uint32_t unsupported_records = 0;
};

// Resolves modulators in exactly the same preset->instrument->sample zone order
// as Sf2ResolvedZoneResolver. Instrument modulators replace an identical
// default/global route; preset modulators are additive after their own
// global/local supersession. Output is control-domain scratch and can be
// released once PlaybackZone[] has been materialized.
class Sf2ZoneModulatorResolver {
public:
    ZoneModulatorResolveStatus resolveProgram(
        Sf2PdtaReader& pdta,
        std::uint16_t bank,
        std::uint16_t program,
        ResolvedZoneModulation* output,
        std::size_t output_capacity,
        ZoneModulatorResolveResult* result);
};

}  // namespace gm_synth::sf2
