#include "Sf2ZoneModulatorResolver.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::size_t kNoZone = std::numeric_limits<std::size_t>::max();
constexpr std::uint16_t kVelocityAttenuationSourceOper = 0x0502u;
constexpr std::uint16_t kVelocityFilterSourceOper = 0x0102u;
constexpr std::uint16_t kChannelVolumeSourceOper = 0x0587u;
constexpr std::uint16_t kChannelPanSourceOper = 0x028Au;
constexpr std::uint16_t kChannelExpressionSourceOper = 0x058Bu;
constexpr std::uint16_t kChannelPressureSourceOper = 0x000Du;
constexpr std::uint16_t kModulationWheelSourceOper = 0x0081u;
constexpr std::uint16_t kInitialAttenuationDestination = 48u;
constexpr std::uint16_t kPanDestination = 17u;
constexpr std::uint16_t kInitialFilterFcDestination = 8u;
constexpr std::uint16_t kVibratoLfoToPitchDestination = 6u;
constexpr std::uint16_t kReverbSourceOper = 0x00DBu;
constexpr std::uint16_t kReverbDestination = 16u;
constexpr std::uint16_t kChorusSourceOper = 0x00DDu;
constexpr std::uint16_t kChorusDestination = 15u;

struct ZoneTableRange {
    std::uint16_t generator_begin = 0;
    std::uint16_t generator_end = 0;
    std::uint16_t modulator_begin = 0;
    std::uint16_t modulator_end = 0;
};

struct KeyVelocityRange {
    int low = 0;
    int high = 127;
};

struct ModulatorScan {
    bool velocity_attenuation_found = false;
    std::int32_t velocity_attenuation_amount = 0;
    bool velocity_filter_found = false;
    std::int32_t velocity_filter_amount = 0;
    bool channel_volume_found = false;
    std::int32_t channel_volume_amount = 0;
    bool channel_pan_found = false;
    std::int32_t channel_pan_amount = 0;
    bool channel_expression_found = false;
    std::int32_t channel_expression_amount = 0;
    bool channel_pressure_vibrato_found = false;
    std::int32_t channel_pressure_vibrato_amount = 0;
    bool modulation_wheel_vibrato_found = false;
    std::int32_t modulation_wheel_vibrato_amount = 0;
    bool reverb_send_found = false;
    std::int32_t reverb_send_amount = 0;
    bool chorus_send_found = false;
    std::int32_t chorus_send_amount = 0;
    std::uint32_t supported_velocity_records = 0;
    std::uint32_t supported_velocity_filter_records = 0;
    std::uint32_t supported_channel_control_records = 0;
    std::uint32_t supported_vibrato_records = 0;
    std::uint32_t supported_effects_send_records = 0;
    std::uint32_t supported_records = 0;
    std::uint32_t unsupported_records = 0;
};

std::uint32_t saturatingAdd32(std::uint32_t a, std::uint32_t b) {
    if (a > std::numeric_limits<std::uint32_t>::max() - b) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return a + b;
}

std::uint16_t clampU16(std::uint32_t value) {
    return value > std::numeric_limits<std::uint16_t>::max()
        ? std::numeric_limits<std::uint16_t>::max()
        : static_cast<std::uint16_t>(value);
}

std::int32_t saturatingAddI32(std::int32_t a, std::int32_t b) {
    const std::int64_t value = static_cast<std::int64_t>(a) + b;
    if (value > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

bool presetZoneRange(
    Sf2PdtaReader& pdta,
    std::size_t zone_index,
    ZoneTableRange* out) {
    if (out == nullptr || zone_index + 1u >= pdta.presetBagRecordCount()) {
        return false;
    }
    Sf2PdtaReader::Bag current{};
    Sf2PdtaReader::Bag next{};
    if (!pdta.presetBag(zone_index, &current) ||
        !pdta.presetBag(zone_index + 1u, &next)) {
        return false;
    }
    if (current.generator_index > next.generator_index ||
        current.modulator_index > next.modulator_index ||
        next.generator_index > pdta.presetGeneratorRecordCount() ||
        next.modulator_index > pdta.presetModulatorRecordCount()) {
        return false;
    }
    *out = ZoneTableRange{
        current.generator_index,
        next.generator_index,
        current.modulator_index,
        next.modulator_index,
    };
    return true;
}

bool instrumentZoneRange(
    Sf2PdtaReader& pdta,
    std::size_t zone_index,
    ZoneTableRange* out) {
    if (out == nullptr || zone_index + 1u >= pdta.instrumentBagRecordCount()) {
        return false;
    }
    Sf2PdtaReader::Bag current{};
    Sf2PdtaReader::Bag next{};
    if (!pdta.instrumentBag(zone_index, &current) ||
        !pdta.instrumentBag(zone_index + 1u, &next)) {
        return false;
    }
    if (current.generator_index > next.generator_index ||
        current.modulator_index > next.modulator_index ||
        next.generator_index > pdta.instrumentGeneratorRecordCount() ||
        next.modulator_index > pdta.instrumentModulatorRecordCount()) {
        return false;
    }
    *out = ZoneTableRange{
        current.generator_index,
        next.generator_index,
        current.modulator_index,
        next.modulator_index,
    };
    return true;
}

bool structuralIndex(
    Sf2PdtaReader& pdta,
    const ZoneTableRange& range,
    bool preset,
    std::uint16_t oper,
    bool* found,
    std::uint16_t* value) {
    if (found == nullptr || value == nullptr) {
        return false;
    }
    *found = false;
    *value = 0;
    for (std::size_t index = range.generator_begin; index < range.generator_end; ++index) {
        Sf2PdtaReader::Generator generator{};
        const bool ok = preset
            ? pdta.presetGenerator(index, &generator)
            : pdta.instrumentGenerator(index, &generator);
        if (!ok) {
            return false;
        }
        if (generator.oper == oper) {
            *found = true;
            *value = generator.amount;
        }
    }
    return true;
}

bool keyVelocityRange(
    Sf2PdtaReader& pdta,
    const ZoneTableRange& range,
    bool preset,
    std::uint16_t oper,
    KeyVelocityRange* out) {
    if (out == nullptr) {
        return false;
    }
    KeyVelocityRange result{};
    for (std::size_t index = range.generator_begin; index < range.generator_end; ++index) {
        Sf2PdtaReader::Generator generator{};
        const bool ok = preset
            ? pdta.presetGenerator(index, &generator)
            : pdta.instrumentGenerator(index, &generator);
        if (!ok) {
            return false;
        }
        if (generator.oper == oper) {
            result.low = std::min<int>(generator.rangeLow(), 127);
            result.high = std::min<int>(generator.rangeHigh(), 127);
        }
    }
    *out = result;
    return true;
}

bool intersectRange(
    const KeyVelocityRange& a,
    const KeyVelocityRange& b,
    KeyVelocityRange* out) {
    const int low = std::max(a.low, b.low);
    const int high = std::min(a.high, b.high);
    if (low > high) {
        return false;
    }
    if (out != nullptr) {
        *out = KeyVelocityRange{low, high};
    }
    return true;
}

bool isTerminalModulator(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == 0 && modulator.destination_oper == 0 &&
           modulator.amount == 0 && modulator.amount_source_oper == 0 &&
           modulator.transform_oper == 0;
}

bool isSupportedVelocityAttenuationRoute(
    const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kVelocityAttenuationSourceOper &&
           modulator.destination_oper == kInitialAttenuationDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedVelocityFilterRoute(
    const Sf2PdtaReader::Modulator& modulator) {
    // SoundFont 2.04 route: velocity, negative-unipolar linear, no secondary.
    return modulator.source_oper == kVelocityFilterSourceOper &&
           modulator.destination_oper == kInitialFilterFcDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isIgnoredSf201VelocityFilterCancellation(
    const Sf2PdtaReader::Modulator& modulator) {
    // Some real banks explicitly cancel the obsolete 2.01 route as well as the
    // 2.04 route. We do not instantiate the 2.01 default; a zero-amount record
    // with its legacy velocity-switch secondary is therefore a supported no-op.
    return modulator.source_oper == kVelocityFilterSourceOper &&
           modulator.destination_oper == kInitialFilterFcDestination &&
           modulator.amount_source_oper == 0x0D02u &&
           modulator.transform_oper == 0 && modulator.amount == 0;
}

bool isSupportedChannelVolumeRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kChannelVolumeSourceOper &&
           modulator.destination_oper == kInitialAttenuationDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedChannelPanRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kChannelPanSourceOper &&
           modulator.destination_oper == kPanDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedChannelExpressionRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kChannelExpressionSourceOper &&
           modulator.destination_oper == kInitialAttenuationDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedChannelPressureVibratoRoute(
    const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kChannelPressureSourceOper &&
           modulator.destination_oper == kVibratoLfoToPitchDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedModulationWheelVibratoRoute(
    const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kModulationWheelSourceOper &&
           modulator.destination_oper == kVibratoLfoToPitchDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedReverbSendRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kReverbSourceOper &&
           modulator.destination_oper == kReverbDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool isSupportedChorusSendRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kChorusSourceOper &&
           modulator.destination_oper == kChorusDestination &&
           modulator.amount_source_oper == 0 && modulator.transform_oper == 0;
}

bool scanModulators(
    Sf2PdtaReader& pdta,
    const ZoneTableRange& range,
    bool preset,
    ModulatorScan* out) {
    if (out == nullptr) {
        return false;
    }
    ModulatorScan scan{};
    for (std::size_t index = range.modulator_begin; index < range.modulator_end; ++index) {
        Sf2PdtaReader::Modulator modulator{};
        const bool ok = preset
            ? pdta.presetModulator(index, &modulator)
            : pdta.instrumentModulator(index, &modulator);
        if (!ok) {
            return false;
        }
        if (isTerminalModulator(modulator)) {
            continue;
        }
        if (isSupportedVelocityAttenuationRoute(modulator)) {
            scan.velocity_attenuation_found = true;
            scan.velocity_attenuation_amount = modulator.amount;
            scan.supported_velocity_records = saturatingAdd32(scan.supported_velocity_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedVelocityFilterRoute(modulator)) {
            scan.velocity_filter_found = true;
            scan.velocity_filter_amount = modulator.amount;
            scan.supported_velocity_filter_records = saturatingAdd32(
                scan.supported_velocity_filter_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isIgnoredSf201VelocityFilterCancellation(modulator)) {
            scan.supported_velocity_filter_records = saturatingAdd32(
                scan.supported_velocity_filter_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedChannelVolumeRoute(modulator)) {
            scan.channel_volume_found = true;
            scan.channel_volume_amount = modulator.amount;
            scan.supported_channel_control_records = saturatingAdd32(
                scan.supported_channel_control_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedChannelPanRoute(modulator)) {
            scan.channel_pan_found = true;
            scan.channel_pan_amount = modulator.amount;
            scan.supported_channel_control_records = saturatingAdd32(
                scan.supported_channel_control_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedChannelExpressionRoute(modulator)) {
            scan.channel_expression_found = true;
            scan.channel_expression_amount = modulator.amount;
            scan.supported_channel_control_records = saturatingAdd32(
                scan.supported_channel_control_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedChannelPressureVibratoRoute(modulator)) {
            scan.channel_pressure_vibrato_found = true;
            scan.channel_pressure_vibrato_amount = modulator.amount;
            scan.supported_vibrato_records = saturatingAdd32(scan.supported_vibrato_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedModulationWheelVibratoRoute(modulator)) {
            scan.modulation_wheel_vibrato_found = true;
            scan.modulation_wheel_vibrato_amount = modulator.amount;
            scan.supported_vibrato_records = saturatingAdd32(scan.supported_vibrato_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedReverbSendRoute(modulator)) {
            scan.reverb_send_found = true;
            scan.reverb_send_amount = modulator.amount;
            scan.supported_effects_send_records = saturatingAdd32(
                scan.supported_effects_send_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else if (isSupportedChorusSendRoute(modulator)) {
            scan.chorus_send_found = true;
            scan.chorus_send_amount = modulator.amount;
            scan.supported_effects_send_records = saturatingAdd32(
                scan.supported_effects_send_records, 1);
            scan.supported_records = saturatingAdd32(scan.supported_records, 1);
        } else {
            scan.unsupported_records = saturatingAdd32(scan.unsupported_records, 1);
        }
    }
    *out = scan;
    return true;
}

ResolvedZoneModulation combine(
    const ModulatorScan& preset_global,
    const ModulatorScan& preset_local,
    const ModulatorScan& instrument_global,
    const ModulatorScan& instrument_local) {
    ResolvedZoneModulation resolved{};

    // Instrument modulators supersede identical mandatory default routes.
    if (instrument_global.velocity_attenuation_found) {
        resolved.velocity_to_attenuation_cb =
            instrument_global.velocity_attenuation_amount;
    }
    if (instrument_local.velocity_attenuation_found) {
        resolved.velocity_to_attenuation_cb =
            instrument_local.velocity_attenuation_amount;
    }
    if (instrument_global.velocity_filter_found) {
        resolved.velocity_to_filter_fc_cents =
            instrument_global.velocity_filter_amount;
    }
    if (instrument_local.velocity_filter_found) {
        resolved.velocity_to_filter_fc_cents =
            instrument_local.velocity_filter_amount;
    }
    if (instrument_global.channel_volume_found) {
        resolved.volume_to_attenuation_cb = instrument_global.channel_volume_amount;
    }
    if (instrument_local.channel_volume_found) {
        resolved.volume_to_attenuation_cb = instrument_local.channel_volume_amount;
    }
    if (instrument_global.channel_pan_found) {
        resolved.pan_amount = instrument_global.channel_pan_amount;
    }
    if (instrument_local.channel_pan_found) {
        resolved.pan_amount = instrument_local.channel_pan_amount;
    }
    if (instrument_global.channel_expression_found) {
        resolved.expression_to_attenuation_cb = instrument_global.channel_expression_amount;
    }
    if (instrument_local.channel_expression_found) {
        resolved.expression_to_attenuation_cb = instrument_local.channel_expression_amount;
    }
    if (instrument_global.channel_pressure_vibrato_found) {
        resolved.channel_pressure_to_vib_lfo_pitch_cents =
            instrument_global.channel_pressure_vibrato_amount;
    }
    if (instrument_local.channel_pressure_vibrato_found) {
        resolved.channel_pressure_to_vib_lfo_pitch_cents =
            instrument_local.channel_pressure_vibrato_amount;
    }
    if (instrument_global.modulation_wheel_vibrato_found) {
        resolved.modulation_wheel_to_vib_lfo_pitch_cents =
            instrument_global.modulation_wheel_vibrato_amount;
    }
    if (instrument_local.modulation_wheel_vibrato_found) {
        resolved.modulation_wheel_to_vib_lfo_pitch_cents =
            instrument_local.modulation_wheel_vibrato_amount;
    }
    if (instrument_global.reverb_send_found) {
        resolved.reverb_send_amount = instrument_global.reverb_send_amount;
    }
    if (instrument_local.reverb_send_found) {
        resolved.reverb_send_amount = instrument_local.reverb_send_amount;
    }
    if (instrument_global.chorus_send_found) {
        resolved.chorus_send_amount = instrument_global.chorus_send_amount;
    }
    if (instrument_local.chorus_send_found) {
        resolved.chorus_send_amount = instrument_local.chorus_send_amount;
    }

    // Preset global/local obey their own supersession rule; each surviving
    // preset route is additive to the instrument/default result.
    bool preset_velocity_found = false;
    std::int32_t preset_velocity_amount = 0;
    if (preset_global.velocity_attenuation_found) {
        preset_velocity_found = true;
        preset_velocity_amount = preset_global.velocity_attenuation_amount;
    }
    if (preset_local.velocity_attenuation_found) {
        preset_velocity_found = true;
        preset_velocity_amount = preset_local.velocity_attenuation_amount;
    }
    if (preset_velocity_found) {
        resolved.velocity_to_attenuation_cb = saturatingAddI32(
            resolved.velocity_to_attenuation_cb,
            preset_velocity_amount);
    }

    bool preset_filter_found = false;
    std::int32_t preset_filter_amount = 0;
    if (preset_global.velocity_filter_found) {
        preset_filter_found = true;
        preset_filter_amount = preset_global.velocity_filter_amount;
    }
    if (preset_local.velocity_filter_found) {
        preset_filter_found = true;
        preset_filter_amount = preset_local.velocity_filter_amount;
    }
    if (preset_filter_found) {
        resolved.velocity_to_filter_fc_cents = saturatingAddI32(
            resolved.velocity_to_filter_fc_cents,
            preset_filter_amount);
    }

    bool preset_volume_found = false;
    std::int32_t preset_volume_amount = 0;
    if (preset_global.channel_volume_found) {
        preset_volume_found = true;
        preset_volume_amount = preset_global.channel_volume_amount;
    }
    if (preset_local.channel_volume_found) {
        preset_volume_found = true;
        preset_volume_amount = preset_local.channel_volume_amount;
    }
    if (preset_volume_found) {
        resolved.volume_to_attenuation_cb = saturatingAddI32(
            resolved.volume_to_attenuation_cb,
            preset_volume_amount);
    }

    bool preset_pan_found = false;
    std::int32_t preset_pan_amount = 0;
    if (preset_global.channel_pan_found) {
        preset_pan_found = true;
        preset_pan_amount = preset_global.channel_pan_amount;
    }
    if (preset_local.channel_pan_found) {
        preset_pan_found = true;
        preset_pan_amount = preset_local.channel_pan_amount;
    }
    if (preset_pan_found) {
        resolved.pan_amount = saturatingAddI32(resolved.pan_amount, preset_pan_amount);
    }

    bool preset_expression_found = false;
    std::int32_t preset_expression_amount = 0;
    if (preset_global.channel_expression_found) {
        preset_expression_found = true;
        preset_expression_amount = preset_global.channel_expression_amount;
    }
    if (preset_local.channel_expression_found) {
        preset_expression_found = true;
        preset_expression_amount = preset_local.channel_expression_amount;
    }
    if (preset_expression_found) {
        resolved.expression_to_attenuation_cb = saturatingAddI32(
            resolved.expression_to_attenuation_cb,
            preset_expression_amount);
    }

    bool preset_pressure_found = false;
    std::int32_t preset_pressure_amount = 0;
    if (preset_global.channel_pressure_vibrato_found) {
        preset_pressure_found = true;
        preset_pressure_amount = preset_global.channel_pressure_vibrato_amount;
    }
    if (preset_local.channel_pressure_vibrato_found) {
        preset_pressure_found = true;
        preset_pressure_amount = preset_local.channel_pressure_vibrato_amount;
    }
    if (preset_pressure_found) {
        resolved.channel_pressure_to_vib_lfo_pitch_cents = saturatingAddI32(
            resolved.channel_pressure_to_vib_lfo_pitch_cents,
            preset_pressure_amount);
    }

    bool preset_modwheel_found = false;
    std::int32_t preset_modwheel_amount = 0;
    if (preset_global.modulation_wheel_vibrato_found) {
        preset_modwheel_found = true;
        preset_modwheel_amount = preset_global.modulation_wheel_vibrato_amount;
    }
    if (preset_local.modulation_wheel_vibrato_found) {
        preset_modwheel_found = true;
        preset_modwheel_amount = preset_local.modulation_wheel_vibrato_amount;
    }
    if (preset_modwheel_found) {
        resolved.modulation_wheel_to_vib_lfo_pitch_cents = saturatingAddI32(
            resolved.modulation_wheel_to_vib_lfo_pitch_cents,
            preset_modwheel_amount);
    }

    bool preset_reverb_found = false;
    std::int32_t preset_reverb_amount = 0;
    if (preset_global.reverb_send_found) {
        preset_reverb_found = true;
        preset_reverb_amount = preset_global.reverb_send_amount;
    }
    if (preset_local.reverb_send_found) {
        preset_reverb_found = true;
        preset_reverb_amount = preset_local.reverb_send_amount;
    }
    if (preset_reverb_found) {
        resolved.reverb_send_amount = saturatingAddI32(
            resolved.reverb_send_amount,
            preset_reverb_amount);
    }

    bool preset_chorus_found = false;
    std::int32_t preset_chorus_amount = 0;
    if (preset_global.chorus_send_found) {
        preset_chorus_found = true;
        preset_chorus_amount = preset_global.chorus_send_amount;
    }
    if (preset_local.chorus_send_found) {
        preset_chorus_found = true;
        preset_chorus_amount = preset_local.chorus_send_amount;
    }
    if (preset_chorus_found) {
        resolved.chorus_send_amount = saturatingAddI32(
            resolved.chorus_send_amount,
            preset_chorus_amount);
    }

    const std::uint32_t supported_velocity = saturatingAdd32(
        saturatingAdd32(
            preset_global.supported_velocity_records,
            preset_local.supported_velocity_records),
        saturatingAdd32(
            instrument_global.supported_velocity_records,
            instrument_local.supported_velocity_records));
    const std::uint32_t supported_filter = saturatingAdd32(
        saturatingAdd32(
            preset_global.supported_velocity_filter_records,
            preset_local.supported_velocity_filter_records),
        saturatingAdd32(
            instrument_global.supported_velocity_filter_records,
            instrument_local.supported_velocity_filter_records));
    const std::uint32_t supported_channel_controls = saturatingAdd32(
        saturatingAdd32(
            preset_global.supported_channel_control_records,
            preset_local.supported_channel_control_records),
        saturatingAdd32(
            instrument_global.supported_channel_control_records,
            instrument_local.supported_channel_control_records));
    const std::uint32_t supported_vibrato = saturatingAdd32(
        saturatingAdd32(
            preset_global.supported_vibrato_records,
            preset_local.supported_vibrato_records),
        saturatingAdd32(
            instrument_global.supported_vibrato_records,
            instrument_local.supported_vibrato_records));
    const std::uint32_t supported_effects = saturatingAdd32(
        saturatingAdd32(
            preset_global.supported_effects_send_records,
            preset_local.supported_effects_send_records),
        saturatingAdd32(
            instrument_global.supported_effects_send_records,
            instrument_local.supported_effects_send_records));
    const std::uint32_t unsupported = saturatingAdd32(
        saturatingAdd32(preset_global.unsupported_records, preset_local.unsupported_records),
        saturatingAdd32(instrument_global.unsupported_records, instrument_local.unsupported_records));
    resolved.supported_velocity_attenuation_records = clampU16(supported_velocity);
    resolved.supported_velocity_filter_records = clampU16(supported_filter);
    resolved.supported_channel_control_records = clampU16(supported_channel_controls);
    resolved.supported_vibrato_records = clampU16(supported_vibrato);
    resolved.supported_effects_send_records = clampU16(supported_effects);
    resolved.unsupported_modulator_records = clampU16(unsupported);
    return resolved;
}

}  // namespace

ZoneModulatorResolveStatus Sf2ZoneModulatorResolver::resolveProgram(
    Sf2PdtaReader& pdta,
    std::uint16_t bank,
    std::uint16_t program,
    ResolvedZoneModulation* output,
    std::size_t output_capacity,
    ZoneModulatorResolveResult* result) {
    if (result == nullptr || (output == nullptr && output_capacity != 0)) {
        if (result != nullptr) {
            *result = ZoneModulatorResolveResult{};
        }
        return ZoneModulatorResolveStatus::InvalidArgument;
    }
    *result = ZoneModulatorResolveResult{};

    std::size_t preset_index = pdta.presetCount();
    Sf2PdtaReader::PresetHeader preset{};
    for (std::size_t index = 0; index < pdta.presetCount(); ++index) {
        Sf2PdtaReader::PresetHeader candidate{};
        if (!pdta.presetHeader(index, &candidate)) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }
        if (candidate.bank == bank && candidate.program == program) {
            preset_index = index;
            preset = candidate;
            break;
        }
    }
    if (preset_index >= pdta.presetCount()) {
        result->status = ZoneModulatorResolveStatus::PresetNotFound;
        return result->status;
    }
    result->preset_index = static_cast<std::uint16_t>(preset_index);

    Sf2PdtaReader::PresetHeader next_preset{};
    if (!pdta.presetHeader(preset_index + 1u, &next_preset) ||
        preset.bag_index > next_preset.bag_index) {
        result->status = ZoneModulatorResolveStatus::ReaderError;
        return result->status;
    }

    std::size_t preset_global_zone = kNoZone;
    for (std::size_t zone_index = preset.bag_index;
         zone_index < next_preset.bag_index;
         ++zone_index) {
        ZoneTableRange range{};
        bool has_instrument = false;
        std::uint16_t instrument_id = 0;
        if (!presetZoneRange(pdta, zone_index, &range) ||
            !structuralIndex(pdta, range, true, 41, &has_instrument, &instrument_id)) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument) {
            preset_global_zone = zone_index;
        }
    }

    KeyVelocityRange preset_global_key{};
    KeyVelocityRange preset_global_velocity{};
    ModulatorScan preset_global_modulators{};
    if (preset_global_zone != kNoZone) {
        ZoneTableRange range{};
        if (!presetZoneRange(pdta, preset_global_zone, &range) ||
            !keyVelocityRange(pdta, range, true, 43, &preset_global_key) ||
            !keyVelocityRange(pdta, range, true, 44, &preset_global_velocity) ||
            !scanModulators(pdta, range, true, &preset_global_modulators)) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }
    }

    std::size_t required = 0;
    std::size_t written = 0;
    for (std::size_t preset_zone = preset.bag_index;
         preset_zone < next_preset.bag_index;
         ++preset_zone) {
        ZoneTableRange preset_local_range{};
        bool has_instrument = false;
        std::uint16_t instrument_id = 0;
        if (!presetZoneRange(pdta, preset_zone, &preset_local_range) ||
            !structuralIndex(
                pdta,
                preset_local_range,
                true,
                41,
                &has_instrument,
                &instrument_id)) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument || static_cast<std::size_t>(instrument_id) >= pdta.instrumentCount()) {
            continue;
        }

        KeyVelocityRange preset_local_key{};
        KeyVelocityRange preset_local_velocity{};
        KeyVelocityRange preset_key{};
        KeyVelocityRange preset_velocity{};
        ModulatorScan preset_local_modulators{};
        if (!keyVelocityRange(pdta, preset_local_range, true, 43, &preset_local_key) ||
            !keyVelocityRange(pdta, preset_local_range, true, 44, &preset_local_velocity) ||
            !scanModulators(pdta, preset_local_range, true, &preset_local_modulators)) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }
        if (!intersectRange(preset_global_key, preset_local_key, &preset_key) ||
            !intersectRange(preset_global_velocity, preset_local_velocity, &preset_velocity)) {
            continue;
        }

        Sf2PdtaReader::InstrumentHeader instrument{};
        Sf2PdtaReader::InstrumentHeader next_instrument{};
        if (!pdta.instrumentHeader(instrument_id, &instrument) ||
            !pdta.instrumentHeader(static_cast<std::size_t>(instrument_id) + 1u, &next_instrument) ||
            instrument.bag_index > next_instrument.bag_index) {
            result->status = ZoneModulatorResolveStatus::ReaderError;
            return result->status;
        }

        std::size_t instrument_global_zone = kNoZone;
        for (std::size_t zone_index = instrument.bag_index;
             zone_index < next_instrument.bag_index;
             ++zone_index) {
            ZoneTableRange range{};
            bool has_sample = false;
            std::uint16_t sample_id = 0;
            if (!instrumentZoneRange(pdta, zone_index, &range) ||
                !structuralIndex(pdta, range, false, 53, &has_sample, &sample_id)) {
                result->status = ZoneModulatorResolveStatus::ReaderError;
                return result->status;
            }
            if (!has_sample) {
                instrument_global_zone = zone_index;
            }
        }

        KeyVelocityRange instrument_global_key{};
        KeyVelocityRange instrument_global_velocity{};
        ModulatorScan instrument_global_modulators{};
        if (instrument_global_zone != kNoZone) {
            ZoneTableRange range{};
            if (!instrumentZoneRange(pdta, instrument_global_zone, &range) ||
                !keyVelocityRange(pdta, range, false, 43, &instrument_global_key) ||
                !keyVelocityRange(pdta, range, false, 44, &instrument_global_velocity) ||
                !scanModulators(pdta, range, false, &instrument_global_modulators)) {
                result->status = ZoneModulatorResolveStatus::ReaderError;
                return result->status;
            }
        }

        for (std::size_t instrument_zone = instrument.bag_index;
             instrument_zone < next_instrument.bag_index;
             ++instrument_zone) {
            ZoneTableRange instrument_local_range{};
            bool has_sample = false;
            std::uint16_t sample_id = 0;
            if (!instrumentZoneRange(pdta, instrument_zone, &instrument_local_range) ||
                !structuralIndex(
                    pdta,
                    instrument_local_range,
                    false,
                    53,
                    &has_sample,
                    &sample_id)) {
                result->status = ZoneModulatorResolveStatus::ReaderError;
                return result->status;
            }
            if (!has_sample || static_cast<std::size_t>(sample_id) >= pdta.sampleCount()) {
                continue;
            }

            KeyVelocityRange instrument_local_key{};
            KeyVelocityRange instrument_local_velocity{};
            KeyVelocityRange instrument_key{};
            KeyVelocityRange instrument_velocity{};
            KeyVelocityRange resolved_key{};
            KeyVelocityRange resolved_velocity{};
            ModulatorScan instrument_local_modulators{};
            if (!keyVelocityRange(pdta, instrument_local_range, false, 43, &instrument_local_key) ||
                !keyVelocityRange(pdta, instrument_local_range, false, 44, &instrument_local_velocity) ||
                !scanModulators(pdta, instrument_local_range, false, &instrument_local_modulators)) {
                result->status = ZoneModulatorResolveStatus::ReaderError;
                return result->status;
            }
            if (!intersectRange(instrument_global_key, instrument_local_key, &instrument_key) ||
                !intersectRange(instrument_global_velocity, instrument_local_velocity, &instrument_velocity) ||
                !intersectRange(preset_key, instrument_key, &resolved_key) ||
                !intersectRange(preset_velocity, instrument_velocity, &resolved_velocity)) {
                continue;
            }

            const ResolvedZoneModulation resolved = combine(
                preset_global_modulators,
                preset_local_modulators,
                instrument_global_modulators,
                instrument_local_modulators);
            result->supported_records = saturatingAdd32(
                result->supported_records,
                saturatingAdd32(
                    saturatingAdd32(
                        resolved.supported_velocity_attenuation_records,
                        resolved.supported_velocity_filter_records),
                    saturatingAdd32(
                        resolved.supported_channel_control_records,
                        saturatingAdd32(
                            resolved.supported_vibrato_records,
                            resolved.supported_effects_send_records))));
            result->unsupported_records = saturatingAdd32(
                result->unsupported_records,
                resolved.unsupported_modulator_records);

            if (required != std::numeric_limits<std::size_t>::max()) {
                ++required;
            }
            if (output != nullptr && written < output_capacity) {
                output[written++] = resolved;
            }
        }
    }

    result->required_capacity = required;
    if (required > output_capacity) {
        result->zone_count = 0;
        result->status = ZoneModulatorResolveStatus::OutputTooSmall;
        return result->status;
    }

    result->zone_count = written;
    result->status = ZoneModulatorResolveStatus::Ok;
    return result->status;
}

}  // namespace gm_synth::sf2