#include "Sf2ZoneModulatorResolver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::size_t kNoZone = std::numeric_limits<std::size_t>::max();
constexpr std::uint16_t kVelocityPositiveLinearSourceOper = 0x0002u;
constexpr std::uint16_t kVelocityNegativeLinearSourceOper = 0x0102u;
constexpr std::uint16_t kVelocityAttenuationSourceOper = 0x0502u;
constexpr std::uint16_t kChannelVolumeSourceOper = 0x0587u;
constexpr std::uint16_t kChannelPanSourceOper = 0x028Au;
constexpr std::uint16_t kChannelExpressionSourceOper = 0x058Bu;
constexpr std::uint16_t kChannelPressureSourceOper = 0x000Du;
constexpr std::uint16_t kModulationWheelSourceOper = 0x0081u;
constexpr std::uint16_t kInitialFilterFcDestination = 8u;
constexpr std::uint16_t kInitialFilterQDestination = 9u;
constexpr std::uint16_t kVibratoLfoToPitchDestination = 6u;
constexpr std::uint16_t kModEnvToFilterFcDestination = 11u;
constexpr std::uint16_t kReverbDestination = 16u;
constexpr std::uint16_t kChorusDestination = 15u;
constexpr std::uint16_t kPanDestination = 17u;
constexpr std::uint16_t kAttackModEnvDestination = 26u;
constexpr std::uint16_t kAttackVolEnvDestination = 34u;
constexpr std::uint16_t kInitialAttenuationDestination = 48u;
constexpr std::uint16_t kReverbSourceOper = 0x00DBu;
constexpr std::uint16_t kChorusSourceOper = 0x00DDu;

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

struct RouteValue {
    bool found = false;
    std::int32_t amount = 0;
};

struct NoteOnRouteSet {
    std::array<Sf2NoteOnSourceRoute, kSf2MaxNoteOnSourceRoutesPerDestination> routes{};
    std::uint8_t count = 0;
};

struct ModulatorScan {
    RouteValue velocity_attenuation{};
    RouteValue velocity_filter{};
    RouteValue velocity_mod_env_filter_positive{};
    RouteValue velocity_mod_env_filter_negative{};
    RouteValue velocity_filter_q{};
    RouteValue velocity_mod_env_attack{};
    RouteValue velocity_vol_env_attack{};
    NoteOnRouteSet initial_filter_fc_note_on{};
    RouteValue channel_volume{};
    RouteValue channel_pan{};
    RouteValue channel_expression{};
    RouteValue channel_pressure_vibrato{};
    RouteValue modulation_wheel_vibrato{};
    RouteValue reverb_send{};
    RouteValue chorus_send{};
    Sf2PreparedNoteOnModulatorSet prepared_note_on{};

    std::uint32_t supported_velocity_records = 0;
    std::uint32_t supported_velocity_filter_records = 0;
    std::uint32_t supported_velocity_dsp_records = 0;
    std::uint32_t supported_note_on_source_records = 0;
    std::uint32_t supported_channel_control_records = 0;
    std::uint32_t supported_vibrato_records = 0;
    std::uint32_t supported_effects_send_records = 0;
    std::uint32_t supported_prepared_note_on_records = 0;
    std::uint32_t unsupported_records = 0;
};

std::uint32_t saturatingAdd32(std::uint32_t a, std::uint32_t b) {
    return a > std::numeric_limits<std::uint32_t>::max() - b
        ? std::numeric_limits<std::uint32_t>::max()
        : a + b;
}

std::uint16_t clampU16(std::uint32_t value) {
    return value > std::numeric_limits<std::uint16_t>::max()
        ? std::numeric_limits<std::uint16_t>::max()
        : static_cast<std::uint16_t>(value);
}

std::int32_t saturatingAddI32(std::int32_t a, std::int32_t b) {
    const std::int64_t value = static_cast<std::int64_t>(a) + b;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
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
        !pdta.presetBag(zone_index + 1u, &next) ||
        current.generator_index > next.generator_index ||
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
        !pdta.instrumentBag(zone_index + 1u, &next) ||
        current.generator_index > next.generator_index ||
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

bool exactRoute(
    const Sf2PdtaReader::Modulator& modulator,
    std::uint16_t source,
    std::uint16_t destination) {
    return modulator.source_oper == source &&
           modulator.destination_oper == destination &&
           modulator.amount_source_oper == 0 &&
           modulator.transform_oper == 0;
}

bool isGenericNoteOnFilterFcRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.destination_oper == kInitialFilterFcDestination &&
        modulator.amount_source_oper == 0 &&
        modulator.transform_oper == 0 &&
        isSupportedSf2NoteOnSourceOper(modulator.source_oper);
}

bool isIgnoredSf201VelocityFilterCancellation(
    const Sf2PdtaReader::Modulator& modulator) {
    return modulator.source_oper == kVelocityNegativeLinearSourceOper &&
           modulator.destination_oper == kInitialFilterFcDestination &&
           modulator.amount_source_oper == 0x0D02u &&
           modulator.transform_oper == 0 &&
           modulator.amount == 0;
}

bool isPreparedNoteOnRuntimeRoute(const Sf2PdtaReader::Modulator& modulator) {
    return modulator.transform_oper == 0 &&
        isSupportedSf2NoteOnSourceOper(modulator.source_oper) &&
        (modulator.amount_source_oper == 0 ||
         isSupportedSf2NoteOnSourceOper(modulator.amount_source_oper)) &&
        isPreparedNoteOnDestination(modulator.destination_oper);
}

Sf2PreparedNoteOnModulator preparedRoute(
    const Sf2PdtaReader::Modulator& modulator) {
    return Sf2PreparedNoteOnModulator{
        modulator.source_oper,
        modulator.destination_oper,
        modulator.amount,
        modulator.amount_source_oper,
    };
}

void remember(RouteValue* route, const Sf2PdtaReader::Modulator& modulator) {
    route->found = true;
    route->amount = modulator.amount;
}

bool replaceNoteOnRoute(
    NoteOnRouteSet* routes,
    std::uint16_t source_oper,
    std::int32_t amount) {
    if (routes == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < routes->count; ++index) {
        if (routes->routes[index].source_oper == source_oper) {
            routes->routes[index].amount = amount;
            return true;
        }
    }
    if (routes->count >= routes->routes.size()) {
        return false;
    }
    routes->routes[routes->count++] = Sf2NoteOnSourceRoute{source_oper, amount};
    return true;
}

bool addNoteOnRoute(
    NoteOnRouteSet* routes,
    const Sf2NoteOnSourceRoute& route) {
    if (routes == nullptr || !route.valid()) {
        return false;
    }
    for (std::size_t index = 0; index < routes->count; ++index) {
        if (routes->routes[index].source_oper == route.source_oper) {
            routes->routes[index].amount = saturatingAddI32(
                routes->routes[index].amount,
                route.amount);
            return true;
        }
    }
    if (routes->count >= routes->routes.size()) {
        return false;
    }
    routes->routes[routes->count++] = route;
    return true;
}

void mergeNoteOnRouteLevel(
    const NoteOnRouteSet& global,
    const NoteOnRouteSet& local,
    NoteOnRouteSet* output,
    std::uint32_t* overflow_records) {
    if (output == nullptr || overflow_records == nullptr) {
        return;
    }
    *output = NoteOnRouteSet{};
    for (std::size_t index = 0; index < global.count; ++index) {
        if (!replaceNoteOnRoute(
                output,
                global.routes[index].source_oper,
                global.routes[index].amount)) {
            *overflow_records = saturatingAdd32(*overflow_records, 1u);
        }
    }
    for (std::size_t index = 0; index < local.count; ++index) {
        if (!replaceNoteOnRoute(
                output,
                local.routes[index].source_oper,
                local.routes[index].amount)) {
            *overflow_records = saturatingAdd32(*overflow_records, 1u);
        }
    }
}

NoteOnRouteSet combineNoteOnRoutes(
    const NoteOnRouteSet& preset_global,
    const NoteOnRouteSet& preset_local,
    const NoteOnRouteSet& instrument_global,
    const NoteOnRouteSet& instrument_local,
    std::uint32_t* overflow_records) {
    NoteOnRouteSet instrument{};
    NoteOnRouteSet preset{};
    NoteOnRouteSet combined{};
    mergeNoteOnRouteLevel(
        instrument_global,
        instrument_local,
        &instrument,
        overflow_records);
    mergeNoteOnRouteLevel(
        preset_global,
        preset_local,
        &preset,
        overflow_records);

    for (std::size_t index = 0; index < instrument.count; ++index) {
        if (!replaceNoteOnRoute(
                &combined,
                instrument.routes[index].source_oper,
                instrument.routes[index].amount)) {
            *overflow_records = saturatingAdd32(*overflow_records, 1u);
        }
    }
    // Preset modulators are additive to the surviving instrument/default
    // modulation. Matching identities therefore add amounts here rather than
    // replacing the instrument contribution.
    for (std::size_t index = 0; index < preset.count; ++index) {
        if (!addNoteOnRoute(&combined, preset.routes[index])) {
            *overflow_records = saturatingAdd32(*overflow_records, 1u);
        }
    }
    return combined;
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

        if (exactRoute(modulator, kVelocityAttenuationSourceOper, kInitialAttenuationDestination)) {
            remember(&scan.velocity_attenuation, modulator);
            ++scan.supported_velocity_records;
        } else if (exactRoute(modulator, kVelocityNegativeLinearSourceOper, kInitialFilterFcDestination)) {
            remember(&scan.velocity_filter, modulator);
            ++scan.supported_velocity_filter_records;
        } else if (isIgnoredSf201VelocityFilterCancellation(modulator)) {
            ++scan.supported_velocity_filter_records;
        } else if (exactRoute(modulator, kVelocityPositiveLinearSourceOper, kModEnvToFilterFcDestination)) {
            remember(&scan.velocity_mod_env_filter_positive, modulator);
            ++scan.supported_velocity_dsp_records;
        } else if (exactRoute(modulator, kVelocityNegativeLinearSourceOper, kModEnvToFilterFcDestination)) {
            remember(&scan.velocity_mod_env_filter_negative, modulator);
            ++scan.supported_velocity_dsp_records;
        } else if (exactRoute(modulator, kVelocityNegativeLinearSourceOper, kInitialFilterQDestination)) {
            remember(&scan.velocity_filter_q, modulator);
            ++scan.supported_velocity_dsp_records;
        } else if (exactRoute(modulator, kVelocityNegativeLinearSourceOper, kAttackModEnvDestination)) {
            remember(&scan.velocity_mod_env_attack, modulator);
            ++scan.supported_velocity_dsp_records;
        } else if (exactRoute(modulator, kVelocityNegativeLinearSourceOper, kAttackVolEnvDestination)) {
            remember(&scan.velocity_vol_env_attack, modulator);
            ++scan.supported_velocity_dsp_records;
        } else if (isGenericNoteOnFilterFcRoute(modulator)) {
            if (replaceNoteOnRoute(
                    &scan.initial_filter_fc_note_on,
                    modulator.source_oper,
                    modulator.amount)) {
                ++scan.supported_note_on_source_records;
            } else {
                ++scan.unsupported_records;
            }
        } else if (exactRoute(modulator, kChannelVolumeSourceOper, kInitialAttenuationDestination)) {
            remember(&scan.channel_volume, modulator);
            ++scan.supported_channel_control_records;
        } else if (exactRoute(modulator, kChannelPanSourceOper, kPanDestination)) {
            remember(&scan.channel_pan, modulator);
            ++scan.supported_channel_control_records;
        } else if (exactRoute(modulator, kChannelExpressionSourceOper, kInitialAttenuationDestination)) {
            remember(&scan.channel_expression, modulator);
            ++scan.supported_channel_control_records;
        } else if (exactRoute(modulator, kChannelPressureSourceOper, kVibratoLfoToPitchDestination)) {
            remember(&scan.channel_pressure_vibrato, modulator);
            ++scan.supported_vibrato_records;
        } else if (exactRoute(modulator, kModulationWheelSourceOper, kVibratoLfoToPitchDestination)) {
            remember(&scan.modulation_wheel_vibrato, modulator);
            ++scan.supported_vibrato_records;
        } else if (exactRoute(modulator, kReverbSourceOper, kReverbDestination)) {
            remember(&scan.reverb_send, modulator);
            ++scan.supported_effects_send_records;
        } else if (exactRoute(modulator, kChorusSourceOper, kChorusDestination)) {
            remember(&scan.chorus_send, modulator);
            ++scan.supported_effects_send_records;
        } else if (isPreparedNoteOnRuntimeRoute(modulator)) {
            if (replacePreparedNoteOnModulator(
                    &scan.prepared_note_on,
                    preparedRoute(modulator))) {
                ++scan.supported_prepared_note_on_records;
            } else {
                ++scan.unsupported_records;
            }
        } else {
            ++scan.unsupported_records;
        }
    }
    *out = scan;
    return true;
}

std::int32_t combinedRoute(
    std::int32_t default_value,
    const RouteValue& preset_global,
    const RouteValue& preset_local,
    const RouteValue& instrument_global,
    const RouteValue& instrument_local) {
    std::int32_t value = default_value;
    if (instrument_global.found) {
        value = instrument_global.amount;
    }
    if (instrument_local.found) {
        value = instrument_local.amount;
    }

    RouteValue preset = preset_global;
    if (preset_local.found) {
        preset = preset_local;
    }
    if (preset.found) {
        value = saturatingAddI32(value, preset.amount);
    }
    return value;
}

std::uint32_t sumCategory(
    std::uint32_t ModulatorScan::*member,
    const ModulatorScan& a,
    const ModulatorScan& b,
    const ModulatorScan& c,
    const ModulatorScan& d) {
    return saturatingAdd32(
        saturatingAdd32(a.*member, b.*member),
        saturatingAdd32(c.*member, d.*member));
}

ResolvedZoneModulation combine(
    const ModulatorScan& preset_global,
    const ModulatorScan& preset_local,
    const ModulatorScan& instrument_global,
    const ModulatorScan& instrument_local) {
    ResolvedZoneModulation resolved{};

    resolved.velocity_to_attenuation_cb = combinedRoute(
        kSf2DefaultVelocityAttenuationCb,
        preset_global.velocity_attenuation,
        preset_local.velocity_attenuation,
        instrument_global.velocity_attenuation,
        instrument_local.velocity_attenuation);
    resolved.velocity_to_filter_fc_cents = combinedRoute(
        kSf2DefaultVelocityFilterCutoffCents,
        preset_global.velocity_filter,
        preset_local.velocity_filter,
        instrument_global.velocity_filter,
        instrument_local.velocity_filter);
    resolved.velocity_positive_to_mod_env_filter_cents = combinedRoute(
        0,
        preset_global.velocity_mod_env_filter_positive,
        preset_local.velocity_mod_env_filter_positive,
        instrument_global.velocity_mod_env_filter_positive,
        instrument_local.velocity_mod_env_filter_positive);
    resolved.velocity_negative_to_mod_env_filter_cents = combinedRoute(
        0,
        preset_global.velocity_mod_env_filter_negative,
        preset_local.velocity_mod_env_filter_negative,
        instrument_global.velocity_mod_env_filter_negative,
        instrument_local.velocity_mod_env_filter_negative);
    resolved.velocity_to_filter_q_cb = combinedRoute(
        0,
        preset_global.velocity_filter_q,
        preset_local.velocity_filter_q,
        instrument_global.velocity_filter_q,
        instrument_local.velocity_filter_q);
    resolved.velocity_to_mod_env_attack_timecents = combinedRoute(
        0,
        preset_global.velocity_mod_env_attack,
        preset_local.velocity_mod_env_attack,
        instrument_global.velocity_mod_env_attack,
        instrument_local.velocity_mod_env_attack);
    resolved.velocity_to_vol_env_attack_timecents = combinedRoute(
        0,
        preset_global.velocity_vol_env_attack,
        preset_local.velocity_vol_env_attack,
        instrument_global.velocity_vol_env_attack,
        instrument_local.velocity_vol_env_attack);

    std::uint32_t route_overflow_records = 0;
    const NoteOnRouteSet note_on_filter_routes = combineNoteOnRoutes(
        preset_global.initial_filter_fc_note_on,
        preset_local.initial_filter_fc_note_on,
        instrument_global.initial_filter_fc_note_on,
        instrument_local.initial_filter_fc_note_on,
        &route_overflow_records);
    resolved.initial_filter_fc_note_on_route_count = note_on_filter_routes.count;
    for (std::size_t index = 0; index < note_on_filter_routes.count; ++index) {
        resolved.initial_filter_fc_note_on_routes[index] = note_on_filter_routes.routes[index];
    }

    resolved.volume_to_attenuation_cb = combinedRoute(
        kSf2DefaultControllerAttenuationCb,
        preset_global.channel_volume,
        preset_local.channel_volume,
        instrument_global.channel_volume,
        instrument_local.channel_volume);
    resolved.pan_amount = combinedRoute(
        kSf2DefaultPanAmount,
        preset_global.channel_pan,
        preset_local.channel_pan,
        instrument_global.channel_pan,
        instrument_local.channel_pan);
    resolved.expression_to_attenuation_cb = combinedRoute(
        kSf2DefaultControllerAttenuationCb,
        preset_global.channel_expression,
        preset_local.channel_expression,
        instrument_global.channel_expression,
        instrument_local.channel_expression);
    resolved.channel_pressure_to_vib_lfo_pitch_cents = combinedRoute(
        kSf2DefaultVibratoControllerAmountCents,
        preset_global.channel_pressure_vibrato,
        preset_local.channel_pressure_vibrato,
        instrument_global.channel_pressure_vibrato,
        instrument_local.channel_pressure_vibrato);
    resolved.modulation_wheel_to_vib_lfo_pitch_cents = combinedRoute(
        kSf2DefaultVibratoControllerAmountCents,
        preset_global.modulation_wheel_vibrato,
        preset_local.modulation_wheel_vibrato,
        instrument_global.modulation_wheel_vibrato,
        instrument_local.modulation_wheel_vibrato);
    resolved.reverb_send_amount = combinedRoute(
        kSf2DefaultEffectsSendAmount,
        preset_global.reverb_send,
        preset_local.reverb_send,
        instrument_global.reverb_send,
        instrument_local.reverb_send);
    resolved.chorus_send_amount = combinedRoute(
        kSf2DefaultEffectsSendAmount,
        preset_global.chorus_send,
        preset_local.chorus_send,
        instrument_global.chorus_send,
        instrument_local.chorus_send);

    const std::uint32_t prepared_records = sumCategory(
        &ModulatorScan::supported_prepared_note_on_records,
        preset_global, preset_local, instrument_global, instrument_local);
    Sf2PreparedNoteOnModulatorSet combined_prepared{};
    bool prepared_ok = combinePreparedNoteOnLevels(
        preset_global.prepared_note_on,
        preset_local.prepared_note_on,
        instrument_global.prepared_note_on,
        instrument_local.prepared_note_on,
        &combined_prepared);
    if (prepared_ok) {
        for (std::size_t index = 0; index < combined_prepared.count; ++index) {
            const Sf2PreparedNoteOnModulator& route = combined_prepared.routes[index];
            if (route.destination_oper == kInitialFilterQDestination) {
                prepared_ok = replacePreparedNoteOnModulator(
                    &resolved.velocity_to_filter_q_cb.additional_routes,
                    route);
            } else if (isRuntimeGenericPreparedNoteOnRoute(route)) {
                prepared_ok = replacePreparedNoteOnModulator(
                    &resolved.prepared_note_on_routes,
                    route);
            } else {
                prepared_ok = false;
            }
            if (!prepared_ok) {
                break;
            }
        }
    }
    if (prepared_ok) {
        resolved.supported_prepared_note_on_records = clampU16(prepared_records);
    } else {
        // Conservative overflow/partition behavior: do not publish a partial
        // route set. Raw records remain explicit unsupported telemetry.
        resolved.velocity_to_filter_q_cb.additional_routes =
            Sf2PreparedNoteOnModulatorSet{};
        resolved.prepared_note_on_routes = Sf2PreparedNoteOnModulatorSet{};
        resolved.supported_prepared_note_on_records = 0;
        route_overflow_records = saturatingAdd32(
            route_overflow_records,
            prepared_records);
    }

    resolved.supported_velocity_attenuation_records = clampU16(sumCategory(
        &ModulatorScan::supported_velocity_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_velocity_filter_records = clampU16(sumCategory(
        &ModulatorScan::supported_velocity_filter_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_velocity_dsp_records = clampU16(sumCategory(
        &ModulatorScan::supported_velocity_dsp_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_note_on_source_records = clampU16(sumCategory(
        &ModulatorScan::supported_note_on_source_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_channel_control_records = clampU16(sumCategory(
        &ModulatorScan::supported_channel_control_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_vibrato_records = clampU16(sumCategory(
        &ModulatorScan::supported_vibrato_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.supported_effects_send_records = clampU16(sumCategory(
        &ModulatorScan::supported_effects_send_records,
        preset_global, preset_local, instrument_global, instrument_local));
    resolved.unsupported_modulator_records = clampU16(saturatingAdd32(
        sumCategory(
            &ModulatorScan::unsupported_records,
            preset_global, preset_local, instrument_global, instrument_local),
        route_overflow_records));
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
            const std::uint32_t velocity_supported = saturatingAdd32(
                saturatingAdd32(
                    resolved.supported_velocity_attenuation_records,
                    resolved.supported_velocity_filter_records),
                resolved.supported_velocity_dsp_records);
            const std::uint32_t other_supported = saturatingAdd32(
                resolved.supported_note_on_source_records,
                saturatingAdd32(
                    resolved.supported_channel_control_records,
                    saturatingAdd32(
                        resolved.supported_vibrato_records,
                        saturatingAdd32(
                            resolved.supported_effects_send_records,
                            resolved.supported_prepared_note_on_records))));
            result->supported_records = saturatingAdd32(
                result->supported_records,
                saturatingAdd32(velocity_supported, other_supported));
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
