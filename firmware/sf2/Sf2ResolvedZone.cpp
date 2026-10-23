#include "Sf2ResolvedZone.h"

#include <algorithm>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::size_t kNoZone = std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t kPresetForbiddenMask =
    (std::uint64_t{1} << 0) |
    (std::uint64_t{1} << 1) |
    (std::uint64_t{1} << 2) |
    (std::uint64_t{1} << 3) |
    (std::uint64_t{1} << 4) |
    (std::uint64_t{1} << 12) |
    (std::uint64_t{1} << 45) |
    (std::uint64_t{1} << 46) |
    (std::uint64_t{1} << 47) |
    (std::uint64_t{1} << 50) |
    (std::uint64_t{1} << 54) |
    (std::uint64_t{1} << 57) |
    (std::uint64_t{1} << 58);

bool structuralGenerator(std::uint16_t oper) {
    return oper == 41 || oper == 43 || oper == 44 || oper == 53 || oper == 60;
}

bool unsignedGenerator(std::uint16_t oper) {
    return oper == 41 || oper == 46 || oper == 47 || oper == 53 ||
           oper == 54 || oper == 57 || oper == 58;
}

bool knownGenerator(std::uint16_t oper) {
    switch (oper) {
        case 0: case 1: case 2: case 3: case 4:
        case 5: case 6: case 7: case 8: case 9:
        case 10: case 11: case 12: case 13:
        case 15: case 16: case 17:
        case 21: case 22: case 23: case 24: case 25:
        case 26: case 27: case 28: case 29: case 30:
        case 31: case 32: case 33: case 34: case 35:
        case 36: case 37: case 38: case 39: case 40:
        case 41: case 43: case 44: case 45: case 46:
        case 47: case 48: case 50: case 51: case 52:
        case 53: case 54: case 56: case 57: case 58:
        case 60:
            return true;
        default:
            return false;
    }
}

std::int32_t generatorValue(const Sf2PdtaReader::Generator& generator) {
    return unsignedGenerator(generator.oper)
        ? static_cast<std::int32_t>(generator.amount)
        : static_cast<std::int32_t>(generator.signedAmount());
}

struct GeneratorRange {
    std::uint16_t generator_begin = 0;
    std::uint16_t generator_end = 0;
    std::uint16_t modulator_begin = 0;
    std::uint16_t modulator_end = 0;
};

struct KeyVelocityRange {
    int low = 0;
    int high = 127;
};

struct RawGeneratorState {
    std::array<std::int32_t, kSf2GeneratorCount> values{};
    std::uint64_t value_mask = 0;
    bool unknown_generator = false;

    bool has(std::uint16_t oper) const {
        return oper < kSf2GeneratorCount &&
               (value_mask & (std::uint64_t{1} << oper)) != 0;
    }

    void set(std::uint16_t oper, std::int32_t value) {
        if (oper >= kSf2GeneratorCount) {
            unknown_generator = true;
            return;
        }
        values[oper] = value;
        value_mask |= std::uint64_t{1} << oper;
    }
};

NativeGeneratorState generatorDefaults() {
    NativeGeneratorState state{};
    const auto set = [&state](std::uint16_t oper, std::int32_t value) {
        state.set(oper, value);
    };

    set(0, 0); set(1, 0); set(2, 0); set(3, 0); set(4, 0);
    set(5, 0); set(6, 0); set(7, 0); set(8, 13500); set(9, 0);
    set(10, 0); set(11, 0); set(12, 0); set(13, 0);
    set(15, 0); set(16, 0); set(17, 0);
    set(21, -12000); set(22, 0); set(23, -12000); set(24, 0);
    set(25, -12000); set(26, -12000); set(27, -12000); set(28, -12000);
    set(29, 0); set(30, -12000); set(31, 0); set(32, 0);
    set(33, -12000); set(34, -12000); set(35, -12000); set(36, -12000);
    set(37, 0); set(38, -12000); set(39, 0); set(40, 0);
    set(45, 0);
    // 46 keynum and 47 velocity intentionally have no numeric default.
    set(48, 0); set(50, 0); set(51, 0); set(52, 0);
    set(54, 0); set(56, 100); set(57, 0);
    // 58 overridingRootKey intentionally has no numeric default.
    return state;
}

bool presetZoneRange(
    Sf2PdtaReader& pdta,
    std::size_t zone_index,
    GeneratorRange* out) {
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
        current.modulator_index > next.modulator_index) {
        return false;
    }
    *out = GeneratorRange{
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
    GeneratorRange* out) {
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
        current.modulator_index > next.modulator_index) {
        return false;
    }
    *out = GeneratorRange{
        current.generator_index,
        next.generator_index,
        current.modulator_index,
        next.modulator_index,
    };
    return true;
}

bool structuralIndex(
    Sf2PdtaReader& pdta,
    const GeneratorRange& range,
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
    const GeneratorRange& range,
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

bool overlayGenerators(
    Sf2PdtaReader& pdta,
    const GeneratorRange& range,
    bool preset,
    RawGeneratorState* state,
    std::uint32_t* unknown_count) {
    if (state == nullptr || unknown_count == nullptr) {
        return false;
    }
    for (std::size_t index = range.generator_begin; index < range.generator_end; ++index) {
        Sf2PdtaReader::Generator generator{};
        const bool ok = preset
            ? pdta.presetGenerator(index, &generator)
            : pdta.instrumentGenerator(index, &generator);
        if (!ok) {
            return false;
        }
        if (!knownGenerator(generator.oper)) {
            state->unknown_generator = true;
            if (*unknown_count != std::numeric_limits<std::uint32_t>::max()) {
                ++(*unknown_count);
            }
        }
        if (structuralGenerator(generator.oper)) {
            continue;
        }
        state->set(generator.oper, generatorValue(generator));
    }
    return true;
}

bool effectiveModulatorCount(
    Sf2PdtaReader& pdta,
    const GeneratorRange& range,
    bool preset,
    std::uint32_t* count) {
    if (count == nullptr) {
        return false;
    }
    std::uint32_t result = 0;
    for (std::size_t index = range.modulator_begin; index < range.modulator_end; ++index) {
        Sf2PdtaReader::Modulator modulator{};
        const bool ok = preset
            ? pdta.presetModulator(index, &modulator)
            : pdta.instrumentModulator(index, &modulator);
        if (!ok) {
            return false;
        }
        const bool effective = modulator.source_oper != 0 ||
            modulator.destination_oper != 0 || modulator.amount != 0 ||
            modulator.amount_source_oper != 0 || modulator.transform_oper != 0;
        if (effective && result != std::numeric_limits<std::uint32_t>::max()) {
            ++result;
        }
    }
    *count = result;
    return true;
}

void applyInstrumentValues(
    const RawGeneratorState& raw,
    NativeGeneratorState* state) {
    for (std::uint16_t oper = 0; oper < kSf2GeneratorCount; ++oper) {
        if (raw.has(oper)) {
            state->set(oper, raw.values[oper]);
        }
    }
}

std::uint64_t applyPresetOffsets(
    const RawGeneratorState& raw,
    NativeGeneratorState* state) {
    std::uint64_t ignored = raw.value_mask & kPresetForbiddenMask;
    for (std::uint16_t oper = 0; oper < kSf2GeneratorCount; ++oper) {
        if (!raw.has(oper) || (kPresetForbiddenMask & (std::uint64_t{1} << oper)) != 0) {
            continue;
        }
        if (state->has(oper)) {
            state->set(oper, state->value(oper) + raw.values[oper]);
        } else {
            state->set(oper, raw.values[oper]);
        }
    }
    return ignored;
}

std::uint32_t saturatingAdd(std::uint32_t a, std::uint32_t b) {
    if (a > std::numeric_limits<std::uint32_t>::max() - b) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return a + b;
}

bool buildZone(
    Sf2PdtaReader& pdta,
    std::uint16_t preset_index,
    std::uint16_t bank,
    std::uint16_t program,
    std::uint16_t instrument_id,
    std::uint16_t sample_id,
    const KeyVelocityRange& key_range,
    const KeyVelocityRange& velocity_range,
    const RawGeneratorState& preset_values,
    const RawGeneratorState& instrument_values,
    std::uint32_t preset_modulators,
    std::uint32_t instrument_modulators,
    NativeResolvedZone* out) {
    if (out == nullptr) {
        return false;
    }

    Sf2PdtaReader::SampleHeader sample{};
    if (!pdta.sampleHeader(sample_id, &sample)) {
        return false;
    }

    NativeResolvedZone zone{};
    zone.preset_index = preset_index;
    zone.bank = bank;
    zone.program = program;
    zone.instrument_id = instrument_id;
    zone.sample_id = sample_id;
    zone.key_low = static_cast<std::uint8_t>(key_range.low);
    zone.key_high = static_cast<std::uint8_t>(key_range.high);
    zone.velocity_low = static_cast<std::uint8_t>(velocity_range.low);
    zone.velocity_high = static_cast<std::uint8_t>(velocity_range.high);
    zone.shdr_start = sample.start;
    zone.shdr_end = sample.end;
    zone.shdr_loop_start = sample.start_loop;
    zone.shdr_loop_end = sample.end_loop;
    zone.sample_rate = sample.sample_rate;
    zone.sample_root_key = sample.original_pitch;
    zone.sample_pitch_correction_cents = sample.pitch_correction;
    zone.sample_link = sample.sample_link;
    zone.sample_type = sample.sample_type;
    zone.preset_modulator_records = preset_modulators;
    zone.instrument_modulator_records = instrument_modulators;

    zone.generators = generatorDefaults();
    applyInstrumentValues(instrument_values, &zone.generators);
    zone.ignored_preset_generator_mask = applyPresetOffsets(preset_values, &zone.generators);

    // A number of widely distributed SF2 banks (FluidR3 is the release
    // qualification example) tag independent sample records as leftSample or
    // rightSample while leaving sampleLink at its zero default. Those records
    // are already referenced independently by the instrument zones. Expanding
    // them as a reciprocal pair would duplicate voices, while rejecting them
    // loses most of the bank. Treat only this specific broken-link shape as an
    // independent mono side and fold the side designation into the authored
    // pan. A genuinely reciprocal pair that happens to link to sample 0 is
    // preserved for the normal stereo-pair path.
    const std::uint16_t source_sample_type = sample.sample_type & 0x7fffu;
    const bool source_is_left = source_sample_type == 4u;
    const bool source_is_right = source_sample_type == 2u;
    bool unlinked_stereo_fallback = false;
    if ((source_is_left || source_is_right) && sample.sample_link == 0u) {
        bool reciprocal_zero_pair = false;
        if (sample_id != 0u) {
            Sf2PdtaReader::SampleHeader sample_zero{};
            if (!pdta.sampleHeader(0u, &sample_zero)) {
                return false;
            }
            const std::uint16_t zero_type = sample_zero.sample_type & 0x7fffu;
            reciprocal_zero_pair =
                sample_zero.sample_link == sample_id &&
                ((source_is_left && zero_type == 2u) ||
                 (source_is_right && zero_type == 4u));
        }
        if (!reciprocal_zero_pair) {
            unlinked_stereo_fallback = true;
            zone.sample_link = 0;
            zone.sample_type = static_cast<std::uint16_t>(
                (sample.sample_type & 0x8000u) | 1u);
            const std::int32_t base_pan = zone.generators.value(17);
            const std::int32_t side_pan = source_is_left ? -500 : 500;
            zone.generators.set(
                17,
                std::clamp<std::int32_t>(base_pan + side_pan, -500, 500));
        }
    }

    const auto gv = [&zone](std::uint16_t oper, std::int32_t fallback = 0) {
        return zone.generators.value(oper, fallback);
    };
    zone.sample_start = static_cast<std::int64_t>(sample.start) +
        gv(0) + static_cast<std::int64_t>(gv(4)) * 32768;
    zone.sample_end = static_cast<std::int64_t>(sample.end) +
        gv(1) + static_cast<std::int64_t>(gv(12)) * 32768;
    zone.loop_start = static_cast<std::int64_t>(sample.start_loop) +
        gv(2) + static_cast<std::int64_t>(gv(45)) * 32768;
    zone.loop_end = static_cast<std::int64_t>(sample.end_loop) +
        gv(3) + static_cast<std::int64_t>(gv(50)) * 32768;

    if (zone.ignored_preset_generator_mask != 0) {
        zone.flags |= NativeZoneFlagIgnoredInvalidPresetGenerator;
    }
    if (preset_values.unknown_generator || instrument_values.unknown_generator) {
        zone.flags |= NativeZoneFlagUnknownGenerator;
    }
    const std::int64_t smpl_points = static_cast<std::int64_t>(pdta.smplPointCount());
    if (zone.sample_start < 0 || zone.sample_end > smpl_points ||
        zone.sample_start >= zone.sample_end) {
        zone.flags |= NativeZoneFlagSampleAddressOutOfSmpl;
    }
    if (zone.sample_start < static_cast<std::int64_t>(sample.start) ||
        zone.sample_end > static_cast<std::int64_t>(sample.end)) {
        zone.flags |= NativeZoneFlagAddressOutsideSampleHeader;
    }
    if ((zone.sampleModes() & 0x3) != 0 &&
        !(zone.sample_start <= zone.loop_start &&
          zone.loop_start < zone.loop_end &&
          zone.loop_end <= zone.sample_end)) {
        zone.flags |= NativeZoneFlagInvalidLoopRange;
    }
    if (zone.rootKey() < 0 || zone.rootKey() > 127) {
        zone.flags |= NativeZoneFlagRootKeyOutOfRange;
    }
    if (sample.isRom()) {
        zone.flags |= NativeZoneFlagRomSample;
    }
    if ((zone.sample_type & 0x7fffu) != 1u) {
        zone.flags |= NativeZoneFlagStereoOrLinkedSample;
    }
    if (unlinked_stereo_fallback) {
        zone.flags |= NativeZoneFlagUnlinkedStereoSideFallback;
    }
    if (preset_modulators != 0) {
        zone.flags |= NativeZoneFlagPresetModulatorsPresent;
    }
    if (instrument_modulators != 0) {
        zone.flags |= NativeZoneFlagInstrumentModulatorsPresent;
    }

    *out = zone;
    return true;
}

}  // namespace

ResolveProgramStatus Sf2ResolvedZoneResolver::resolveProgram(
    Sf2PdtaReader& pdta,
    std::uint16_t bank,
    std::uint16_t program,
    NativeResolvedZone* output,
    std::size_t output_capacity,
    ResolveProgramResult* result) {
    if (result == nullptr || (output == nullptr && output_capacity != 0)) {
        if (result != nullptr) {
            *result = ResolveProgramResult{};
            result->status = ResolveProgramStatus::InvalidArgument;
        }
        return ResolveProgramStatus::InvalidArgument;
    }

    *result = ResolveProgramResult{};

    std::size_t preset_index = pdta.presetCount();
    Sf2PdtaReader::PresetHeader preset_header{};
    for (std::size_t index = 0; index < pdta.presetCount(); ++index) {
        Sf2PdtaReader::PresetHeader candidate{};
        if (!pdta.presetHeader(index, &candidate)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        if (candidate.bank == bank && candidate.program == program) {
            preset_index = index;
            preset_header = candidate;
            break;
        }
    }
    if (preset_index >= pdta.presetCount()) {
        result->status = ResolveProgramStatus::PresetNotFound;
        return result->status;
    }

    Sf2PdtaReader::PresetHeader next_preset{};
    if (!pdta.presetHeader(preset_index + 1u, &next_preset)) {
        result->status = ResolveProgramStatus::ReaderError;
        return result->status;
    }
    if (preset_header.bag_index > next_preset.bag_index) {
        result->status = ResolveProgramStatus::ReaderError;
        return result->status;
    }

    result->preset_index = static_cast<std::uint16_t>(preset_index);

    std::size_t preset_global_zone = kNoZone;
    for (std::size_t zone_index = preset_header.bag_index;
         zone_index < next_preset.bag_index;
         ++zone_index) {
        GeneratorRange range{};
        if (!presetZoneRange(pdta, zone_index, &range)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        bool has_instrument = false;
        std::uint16_t instrument_id = 0;
        if (!structuralIndex(pdta, range, true, 41, &has_instrument, &instrument_id)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument) {
            preset_global_zone = zone_index;
        }
    }

    GeneratorRange preset_global_range{};
    RawGeneratorState preset_global_values{};
    KeyVelocityRange preset_global_key{};
    KeyVelocityRange preset_global_velocity{};
    std::uint32_t preset_global_modulators = 0;
    if (preset_global_zone != kNoZone) {
        if (!presetZoneRange(pdta, preset_global_zone, &preset_global_range) ||
            !overlayGenerators(
                pdta,
                preset_global_range,
                true,
                &preset_global_values,
                &result->unknown_generators) ||
            !keyVelocityRange(pdta, preset_global_range, true, 43, &preset_global_key) ||
            !keyVelocityRange(pdta, preset_global_range, true, 44, &preset_global_velocity) ||
            !effectiveModulatorCount(
                pdta,
                preset_global_range,
                true,
                &preset_global_modulators)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
    }

    std::size_t required_capacity = 0;
    std::size_t written = 0;

    for (std::size_t preset_zone = preset_header.bag_index;
         preset_zone < next_preset.bag_index;
         ++preset_zone) {
        GeneratorRange preset_local_range{};
        if (!presetZoneRange(pdta, preset_zone, &preset_local_range)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }

        bool has_instrument = false;
        std::uint16_t instrument_id = 0;
        if (!structuralIndex(
                pdta,
                preset_local_range,
                true,
                41,
                &has_instrument,
                &instrument_id)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument) {
            continue;
        }
        if (static_cast<std::size_t>(instrument_id) >= pdta.instrumentCount()) {
            if (result->invalid_references != std::numeric_limits<std::uint32_t>::max()) {
                ++result->invalid_references;
            }
            continue;
        }

        KeyVelocityRange preset_local_key{};
        KeyVelocityRange preset_local_velocity{};
        KeyVelocityRange preset_key{};
        KeyVelocityRange preset_velocity{};
        if (!keyVelocityRange(pdta, preset_local_range, true, 43, &preset_local_key) ||
            !keyVelocityRange(pdta, preset_local_range, true, 44, &preset_local_velocity)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        if (!intersectRange(preset_global_key, preset_local_key, &preset_key) ||
            !intersectRange(
                preset_global_velocity,
                preset_local_velocity,
                &preset_velocity)) {
            continue;
        }

        RawGeneratorState preset_values = preset_global_values;
        if (!overlayGenerators(
                pdta,
                preset_local_range,
                true,
                &preset_values,
                &result->unknown_generators)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        std::uint32_t preset_local_modulators = 0;
        if (!effectiveModulatorCount(
                pdta,
                preset_local_range,
                true,
                &preset_local_modulators)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        const std::uint32_t preset_modulators = saturatingAdd(
            preset_global_modulators,
            preset_local_modulators);

        Sf2PdtaReader::InstrumentHeader instrument{};
        Sf2PdtaReader::InstrumentHeader next_instrument{};
        if (!pdta.instrumentHeader(instrument_id, &instrument) ||
            !pdta.instrumentHeader(static_cast<std::size_t>(instrument_id) + 1u, &next_instrument)) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }
        if (instrument.bag_index > next_instrument.bag_index) {
            result->status = ResolveProgramStatus::ReaderError;
            return result->status;
        }

        std::size_t instrument_global_zone = kNoZone;
        for (std::size_t zone_index = instrument.bag_index;
             zone_index < next_instrument.bag_index;
             ++zone_index) {
            GeneratorRange range{};
            if (!instrumentZoneRange(pdta, zone_index, &range)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            bool has_sample = false;
            std::uint16_t sample_id = 0;
            if (!structuralIndex(pdta, range, false, 53, &has_sample, &sample_id)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            if (!has_sample) {
                instrument_global_zone = zone_index;
            }
        }

        GeneratorRange instrument_global_range{};
        RawGeneratorState instrument_global_values{};
        KeyVelocityRange instrument_global_key{};
        KeyVelocityRange instrument_global_velocity{};
        std::uint32_t instrument_global_modulators = 0;
        if (instrument_global_zone != kNoZone) {
            if (!instrumentZoneRange(pdta, instrument_global_zone, &instrument_global_range) ||
                !overlayGenerators(
                    pdta,
                    instrument_global_range,
                    false,
                    &instrument_global_values,
                    &result->unknown_generators) ||
                !keyVelocityRange(
                    pdta,
                    instrument_global_range,
                    false,
                    43,
                    &instrument_global_key) ||
                !keyVelocityRange(
                    pdta,
                    instrument_global_range,
                    false,
                    44,
                    &instrument_global_velocity) ||
                !effectiveModulatorCount(
                    pdta,
                    instrument_global_range,
                    false,
                    &instrument_global_modulators)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
        }

        for (std::size_t instrument_zone = instrument.bag_index;
             instrument_zone < next_instrument.bag_index;
             ++instrument_zone) {
            GeneratorRange instrument_local_range{};
            if (!instrumentZoneRange(pdta, instrument_zone, &instrument_local_range)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }

            bool has_sample = false;
            std::uint16_t sample_id = 0;
            if (!structuralIndex(
                    pdta,
                    instrument_local_range,
                    false,
                    53,
                    &has_sample,
                    &sample_id)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            if (!has_sample) {
                continue;
            }
            if (static_cast<std::size_t>(sample_id) >= pdta.sampleCount()) {
                if (result->invalid_references != std::numeric_limits<std::uint32_t>::max()) {
                    ++result->invalid_references;
                }
                continue;
            }

            KeyVelocityRange instrument_local_key{};
            KeyVelocityRange instrument_local_velocity{};
            KeyVelocityRange instrument_key{};
            KeyVelocityRange instrument_velocity{};
            KeyVelocityRange resolved_key{};
            KeyVelocityRange resolved_velocity{};
            if (!keyVelocityRange(
                    pdta,
                    instrument_local_range,
                    false,
                    43,
                    &instrument_local_key) ||
                !keyVelocityRange(
                    pdta,
                    instrument_local_range,
                    false,
                    44,
                    &instrument_local_velocity)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            if (!intersectRange(
                    instrument_global_key,
                    instrument_local_key,
                    &instrument_key) ||
                !intersectRange(
                    instrument_global_velocity,
                    instrument_local_velocity,
                    &instrument_velocity) ||
                !intersectRange(preset_key, instrument_key, &resolved_key) ||
                !intersectRange(
                    preset_velocity,
                    instrument_velocity,
                    &resolved_velocity)) {
                continue;
            }

            RawGeneratorState instrument_values = instrument_global_values;
            if (!overlayGenerators(
                    pdta,
                    instrument_local_range,
                    false,
                    &instrument_values,
                    &result->unknown_generators)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            std::uint32_t instrument_local_modulators = 0;
            if (!effectiveModulatorCount(
                    pdta,
                    instrument_local_range,
                    false,
                    &instrument_local_modulators)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }
            const std::uint32_t instrument_modulators = saturatingAdd(
                instrument_global_modulators,
                instrument_local_modulators);

            NativeResolvedZone zone{};
            if (!buildZone(
                    pdta,
                    static_cast<std::uint16_t>(preset_index),
                    bank,
                    program,
                    instrument_id,
                    sample_id,
                    resolved_key,
                    resolved_velocity,
                    preset_values,
                    instrument_values,
                    preset_modulators,
                    instrument_modulators,
                    &zone)) {
                result->status = ResolveProgramStatus::ReaderError;
                return result->status;
            }

            if ((zone.flags &
                 (NativeZoneFlagPresetModulatorsPresent |
                  NativeZoneFlagInstrumentModulatorsPresent)) != 0) {
                if (result->zones_with_modulators != std::numeric_limits<std::uint32_t>::max()) {
                    ++result->zones_with_modulators;
                }
            }

            if (required_capacity != std::numeric_limits<std::size_t>::max()) {
                ++required_capacity;
            }
            if (output != nullptr && written < output_capacity) {
                output[written++] = zone;
            }
        }
    }

    result->required_capacity = required_capacity;
    if (required_capacity > output_capacity) {
        result->zone_count = 0;
        result->status = ResolveProgramStatus::OutputTooSmall;
        return result->status;
    }

    result->zone_count = written;
    result->status = ResolveProgramStatus::Ok;
    return result->status;
}

}  // namespace gm_synth::sf2
