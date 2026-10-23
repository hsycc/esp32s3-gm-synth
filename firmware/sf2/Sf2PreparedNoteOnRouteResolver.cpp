#include "Sf2PreparedNoteOnRouteResolver.h"

#include "Sf2NoteOnModulatorTargets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::size_t kNoZone = std::numeric_limits<std::size_t>::max();

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

struct RouteScan {
    Sf2PreparedNoteOnRouteBundle routes{};
    std::uint32_t candidate_records = 0;
    std::uint32_t overflow_records = 0;
};

std::uint32_t saturatingAdd32(std::uint32_t a, std::uint32_t b) {
    return a > std::numeric_limits<std::uint32_t>::max() - b
        ? std::numeric_limits<std::uint32_t>::max()
        : a + b;
}

std::int32_t saturatingAddI32(std::int32_t a, std::int32_t b) {
    const std::int64_t sum = static_cast<std::int64_t>(a) + b;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        sum,
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

bool terminal(const Sf2PdtaReader::Modulator& route) {
    return route.source_oper == 0 && route.destination_oper == 0 &&
        route.amount == 0 && route.amount_source_oper == 0 &&
        route.transform_oper == 0;
}

bool exactLegacyRoute(
    const Sf2PdtaReader::Modulator& route,
    std::uint16_t source,
    std::uint16_t destination) {
    return route.source_oper == source &&
        route.destination_oper == destination &&
        route.amount_source_oper == 0 &&
        route.transform_oper == 0;
}

bool alreadyHandledByLegacyResolver(const Sf2PdtaReader::Modulator& route) {
    // Only identities which also belong to the new prepared-target destination
    // family need exclusion here. Other legacy destinations never enter the
    // candidate predicate below.
    return exactLegacyRoute(route, 0x0102u, 9u) ||
        exactLegacyRoute(route, 0x0002u, 11u) ||
        exactLegacyRoute(route, 0x0102u, 11u) ||
        exactLegacyRoute(route, 0x0102u, 34u) ||
        exactLegacyRoute(route, 0x0502u, 48u);
}

bool candidateRoute(
    const Sf2PdtaReader::Modulator& route,
    Sf2PreparedNoteOnModulator* prepared) {
    if (prepared == nullptr || terminal(route) ||
        route.transform_oper != 0 ||
        !isPreparedNoteOnDestination(route.destination_oper) ||
        !isSupportedSf2NoteOnSourceOper(route.source_oper) ||
        (route.amount_source_oper != 0 &&
         !isSupportedSf2NoteOnSourceOper(route.amount_source_oper)) ||
        alreadyHandledByLegacyResolver(route)) {
        return false;
    }
    *prepared = Sf2PreparedNoteOnModulator{
        route.source_oper,
        route.destination_oper,
        route.amount,
        route.amount_source_oper,
    };
    return prepared->valid();
}

bool replaceRoute(
    Sf2PreparedNoteOnRouteBundle* bundle,
    const Sf2PreparedNoteOnModulator& route) {
    if (bundle == nullptr || !route.valid()) {
        return false;
    }
    for (std::size_t index = 0; index < bundle->count; ++index) {
        if (bundle->routes[index].sameIdentity(route)) {
            bundle->routes[index] = route;
            return true;
        }
    }
    if (bundle->count >= bundle->routes.size()) {
        return false;
    }
    bundle->routes[bundle->count++] = route;
    return true;
}

bool addRoute(
    Sf2PreparedNoteOnRouteBundle* bundle,
    const Sf2PreparedNoteOnModulator& route) {
    if (bundle == nullptr || !route.valid()) {
        return false;
    }
    for (std::size_t index = 0; index < bundle->count; ++index) {
        if (bundle->routes[index].sameIdentity(route)) {
            bundle->routes[index].amount = saturatingAddI32(
                bundle->routes[index].amount,
                route.amount);
            return true;
        }
    }
    if (bundle->count >= bundle->routes.size()) {
        return false;
    }
    bundle->routes[bundle->count++] = route;
    return true;
}

bool scanRoutes(
    Sf2PdtaReader& pdta,
    const ZoneTableRange& range,
    bool preset,
    RouteScan* out) {
    if (out == nullptr) {
        return false;
    }
    RouteScan scan{};
    for (std::size_t index = range.modulator_begin; index < range.modulator_end; ++index) {
        Sf2PdtaReader::Modulator raw{};
        const bool ok = preset
            ? pdta.presetModulator(index, &raw)
            : pdta.instrumentModulator(index, &raw);
        if (!ok) {
            return false;
        }
        Sf2PreparedNoteOnModulator prepared{};
        if (!candidateRoute(raw, &prepared)) {
            continue;
        }
        scan.candidate_records = saturatingAdd32(scan.candidate_records, 1u);
        if (!replaceRoute(&scan.routes, prepared)) {
            scan.overflow_records = saturatingAdd32(scan.overflow_records, 1u);
        }
    }
    *out = scan;
    return true;
}

void mergeLevel(
    const RouteScan& global,
    const RouteScan& local,
    Sf2PreparedNoteOnRouteBundle* output,
    std::uint32_t* overflow) {
    if (output == nullptr || overflow == nullptr) {
        return;
    }
    *output = Sf2PreparedNoteOnRouteBundle{};
    for (std::size_t index = 0; index < global.routes.count; ++index) {
        if (!replaceRoute(output, global.routes.routes[index])) {
            *overflow = saturatingAdd32(*overflow, 1u);
        }
    }
    for (std::size_t index = 0; index < local.routes.count; ++index) {
        if (!replaceRoute(output, local.routes.routes[index])) {
            *overflow = saturatingAdd32(*overflow, 1u);
        }
    }
}

Sf2PreparedNoteOnRouteBundle combine(
    const RouteScan& preset_global,
    const RouteScan& preset_local,
    const RouteScan& instrument_global,
    const RouteScan& instrument_local,
    std::uint32_t* overflow) {
    Sf2PreparedNoteOnRouteBundle preset{};
    Sf2PreparedNoteOnRouteBundle instrument{};
    Sf2PreparedNoteOnRouteBundle combined{};
    mergeLevel(instrument_global, instrument_local, &instrument, overflow);
    mergeLevel(preset_global, preset_local, &preset, overflow);
    for (std::size_t index = 0; index < instrument.count; ++index) {
        if (!replaceRoute(&combined, instrument.routes[index])) {
            *overflow = saturatingAdd32(*overflow, 1u);
        }
    }
    for (std::size_t index = 0; index < preset.count; ++index) {
        if (!addRoute(&combined, preset.routes[index])) {
            *overflow = saturatingAdd32(*overflow, 1u);
        }
    }
    return combined;
}

std::uint32_t recordCount(
    const RouteScan& a,
    const RouteScan& b,
    const RouteScan& c,
    const RouteScan& d) {
    return saturatingAdd32(
        saturatingAdd32(a.candidate_records, b.candidate_records),
        saturatingAdd32(c.candidate_records, d.candidate_records));
}

std::uint32_t scanOverflowCount(
    const RouteScan& a,
    const RouteScan& b,
    const RouteScan& c,
    const RouteScan& d) {
    return saturatingAdd32(
        saturatingAdd32(a.overflow_records, b.overflow_records),
        saturatingAdd32(c.overflow_records, d.overflow_records));
}

}  // namespace

PreparedNoteOnRouteResolveStatus Sf2PreparedNoteOnRouteResolver::resolveProgram(
    Sf2PdtaReader& pdta,
    std::uint16_t bank,
    std::uint16_t program,
    Sf2PreparedNoteOnRouteBundle* output,
    std::size_t output_capacity,
    PreparedNoteOnRouteResolveResult* result) {
    if (result == nullptr || (output == nullptr && output_capacity != 0)) {
        if (result != nullptr) {
            *result = PreparedNoteOnRouteResolveResult{};
        }
        return PreparedNoteOnRouteResolveStatus::InvalidArgument;
    }
    *result = PreparedNoteOnRouteResolveResult{};

    std::size_t preset_index = pdta.presetCount();
    Sf2PdtaReader::PresetHeader preset{};
    for (std::size_t index = 0; index < pdta.presetCount(); ++index) {
        Sf2PdtaReader::PresetHeader candidate{};
        if (!pdta.presetHeader(index, &candidate)) {
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
            return result->status;
        }
        if (candidate.bank == bank && candidate.program == program) {
            preset_index = index;
            preset = candidate;
            break;
        }
    }
    if (preset_index >= pdta.presetCount()) {
        result->status = PreparedNoteOnRouteResolveStatus::PresetNotFound;
        return result->status;
    }
    result->preset_index = static_cast<std::uint16_t>(preset_index);

    Sf2PdtaReader::PresetHeader next_preset{};
    if (!pdta.presetHeader(preset_index + 1u, &next_preset) ||
        preset.bag_index > next_preset.bag_index) {
        result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument) {
            preset_global_zone = zone_index;
        }
    }

    KeyVelocityRange preset_global_key{};
    KeyVelocityRange preset_global_velocity{};
    RouteScan preset_global_routes{};
    if (preset_global_zone != kNoZone) {
        ZoneTableRange range{};
        if (!presetZoneRange(pdta, preset_global_zone, &range) ||
            !keyVelocityRange(pdta, range, true, 43, &preset_global_key) ||
            !keyVelocityRange(pdta, range, true, 44, &preset_global_velocity) ||
            !scanRoutes(pdta, range, true, &preset_global_routes)) {
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
            return result->status;
        }
        if (!has_instrument || static_cast<std::size_t>(instrument_id) >= pdta.instrumentCount()) {
            continue;
        }

        KeyVelocityRange preset_local_key{};
        KeyVelocityRange preset_local_velocity{};
        KeyVelocityRange preset_key{};
        KeyVelocityRange preset_velocity{};
        RouteScan preset_local_routes{};
        if (!keyVelocityRange(pdta, preset_local_range, true, 43, &preset_local_key) ||
            !keyVelocityRange(pdta, preset_local_range, true, 44, &preset_local_velocity) ||
            !scanRoutes(pdta, preset_local_range, true, &preset_local_routes)) {
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
            result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
                result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
                return result->status;
            }
            if (!has_sample) {
                instrument_global_zone = zone_index;
            }
        }

        KeyVelocityRange instrument_global_key{};
        KeyVelocityRange instrument_global_velocity{};
        RouteScan instrument_global_routes{};
        if (instrument_global_zone != kNoZone) {
            ZoneTableRange range{};
            if (!instrumentZoneRange(pdta, instrument_global_zone, &range) ||
                !keyVelocityRange(pdta, range, false, 43, &instrument_global_key) ||
                !keyVelocityRange(pdta, range, false, 44, &instrument_global_velocity) ||
                !scanRoutes(pdta, range, false, &instrument_global_routes)) {
                result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
                result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
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
            RouteScan instrument_local_routes{};
            if (!keyVelocityRange(pdta, instrument_local_range, false, 43, &instrument_local_key) ||
                !keyVelocityRange(pdta, instrument_local_range, false, 44, &instrument_local_velocity) ||
                !scanRoutes(pdta, instrument_local_range, false, &instrument_local_routes)) {
                result->status = PreparedNoteOnRouteResolveStatus::ReaderError;
                return result->status;
            }
            if (!intersectRange(instrument_global_key, instrument_local_key, &instrument_key) ||
                !intersectRange(instrument_global_velocity, instrument_local_velocity, &instrument_velocity) ||
                !intersectRange(preset_key, instrument_key, &resolved_key) ||
                !intersectRange(preset_velocity, instrument_velocity, &resolved_velocity)) {
                continue;
            }

            std::uint32_t combine_overflow = scanOverflowCount(
                preset_global_routes,
                preset_local_routes,
                instrument_global_routes,
                instrument_local_routes);
            const Sf2PreparedNoteOnRouteBundle combined = combine(
                preset_global_routes,
                preset_local_routes,
                instrument_global_routes,
                instrument_local_routes,
                &combine_overflow);
            result->candidate_records = saturatingAdd32(
                result->candidate_records,
                recordCount(
                    preset_global_routes,
                    preset_local_routes,
                    instrument_global_routes,
                    instrument_local_routes));
            result->bundle_overflow_records = saturatingAdd32(
                result->bundle_overflow_records,
                combine_overflow);
            result->max_routes_per_zone = std::max<std::uint16_t>(
                result->max_routes_per_zone,
                combined.count);

            if (required != std::numeric_limits<std::size_t>::max()) {
                ++required;
            }
            if (output != nullptr && written < output_capacity) {
                output[written++] = combined;
            }
        }
    }

    result->required_capacity = required;
    if (required > output_capacity) {
        result->zone_count = 0;
        result->status = PreparedNoteOnRouteResolveStatus::OutputTooSmall;
        return result->status;
    }

    result->zone_count = written;
    result->status = PreparedNoteOnRouteResolveStatus::Ok;
    return result->status;
}

}  // namespace gm_synth::sf2
