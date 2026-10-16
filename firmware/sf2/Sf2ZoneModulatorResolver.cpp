#include "Sf2ZoneModulatorResolver.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::size_t kNoZone = std::numeric_limits<std::size_t>::max();
constexpr std::uint16_t kVelocityAttenuationSourceOper = 0x0502u;
constexpr std::uint16_t kInitialAttenuationDestination = 48u;

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

    // Instrument modulators supersede an identical mandatory default route.
    if (instrument_global.velocity_attenuation_found) {
        resolved.velocity_to_attenuation_cb =
            instrument_global.velocity_attenuation_amount;
    }
    if (instrument_local.velocity_attenuation_found) {
        resolved.velocity_to_attenuation_cb =
            instrument_local.velocity_attenuation_amount;
    }

    // Preset global/local obey their own supersession rule; the surviving
    // preset route is then additive to the instrument/default result.
    bool preset_found = false;
    std::int32_t preset_amount = 0;
    if (preset_global.velocity_attenuation_found) {
        preset_found = true;
        preset_amount = preset_global.velocity_attenuation_amount;
    }
    if (preset_local.velocity_attenuation_found) {
        preset_found = true;
        preset_amount = preset_local.velocity_attenuation_amount;
    }
    if (preset_found) {
        resolved.velocity_to_attenuation_cb = saturatingAddI32(
            resolved.velocity_to_attenuation_cb,
            preset_amount);
    }

    const std::uint32_t supported = saturatingAdd32(
        saturatingAdd32(preset_global.supported_records, preset_local.supported_records),
        saturatingAdd32(instrument_global.supported_records, instrument_local.supported_records));
    const std::uint32_t unsupported = saturatingAdd32(
        saturatingAdd32(preset_global.unsupported_records, preset_local.unsupported_records),
        saturatingAdd32(instrument_global.unsupported_records, instrument_local.unsupported_records));
    resolved.supported_velocity_attenuation_records = clampU16(supported);
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
                resolved.supported_velocity_attenuation_records);
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
