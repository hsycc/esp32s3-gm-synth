#include "Sf2PlaybackProgramPreparer.h"

#include <limits>

namespace gm_synth::sf2 {
namespace {

std::size_t uniqueDependencyCount(
    const NativeResolvedZone* zones,
    std::size_t zone_count) {
    std::size_t unique = 0;
    for (std::size_t index = 0; index < zone_count; ++index) {
        bool seen = false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (zones[previous].sample_id == zones[index].sample_id) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++unique;
        }
    }
    return unique;
}

void writeUniqueDependencies(
    const NativeResolvedZone* zones,
    std::size_t zone_count,
    sample::SampleId* dependencies) {
    std::size_t written = 0;
    for (std::size_t index = 0; index < zone_count; ++index) {
        bool seen = false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (zones[previous].sample_id == zones[index].sample_id) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            dependencies[written++] = zones[index].sample_id;
        }
    }
}

}  // namespace

PlaybackPrepareResult Sf2PlaybackProgramPreparer::prepare(
    Sf2PdtaReader& pdta,
    const PlaybackPrepareRequest& request) const {
    PlaybackPrepareResult result{};
    if (request.channel >= 16 || request.token == 0 ||
        request.bank_generation == 0 || request.snapshot == nullptr ||
        (request.native_capacity != 0 && request.native_scratch == nullptr) ||
        (request.modulation_capacity != 0 && request.modulation_scratch == nullptr) ||
        (request.playback_capacity != 0 && request.playback_zones == nullptr) ||
        (request.dependency_capacity != 0 && request.dependencies == nullptr)) {
        result.status = PlaybackPrepareStatus::InvalidArgument;
        return result;
    }

    Sf2ResolvedZoneResolver resolver;
    ResolveProgramResult count_result{};
    const ResolveProgramStatus count_status = resolver.resolveProgram(
        pdta,
        request.bank,
        request.program,
        nullptr,
        0,
        &count_result);
    result.resolve_status = count_status;
    if (count_status != ResolveProgramStatus::Ok &&
        count_status != ResolveProgramStatus::OutputTooSmall) {
        result.status = PlaybackPrepareStatus::ResolveFailed;
        return result;
    }

    const std::size_t required_zones = count_status == ResolveProgramStatus::OutputTooSmall
        ? count_result.required_capacity
        : count_result.zone_count;
    result.required_zone_capacity = required_zones;

    if (required_zones > gm::PlaybackProgramSnapshot::kMaxZones ||
        required_zones > std::numeric_limits<std::uint16_t>::max() ||
        required_zones > request.native_capacity ||
        required_zones > request.modulation_capacity ||
        required_zones > request.playback_capacity ||
        (required_zones != 0 &&
         (request.native_scratch == nullptr || request.modulation_scratch == nullptr ||
          request.playback_zones == nullptr))) {
        result.status = PlaybackPrepareStatus::ZoneCapacityExceeded;
        return result;
    }

    ResolveProgramResult resolve_result{};
    const ResolveProgramStatus resolve_status = resolver.resolveProgram(
        pdta,
        request.bank,
        request.program,
        request.native_scratch,
        request.native_capacity,
        &resolve_result);
    result.resolve_status = resolve_status;
    if (resolve_status != ResolveProgramStatus::Ok ||
        resolve_result.zone_count != required_zones) {
        result.status = PlaybackPrepareStatus::ResolveFailed;
        return result;
    }
    result.zone_count = resolve_result.zone_count;

    Sf2ZoneModulatorResolver modulator_resolver;
    ZoneModulatorResolveResult modulator_result{};
    const ZoneModulatorResolveStatus modulator_status = modulator_resolver.resolveProgram(
        pdta,
        request.bank,
        request.program,
        request.modulation_scratch,
        request.modulation_capacity,
        &modulator_result);
    result.modulator_resolve_status = modulator_status;
    result.supported_modulator_records = modulator_result.supported_records;
    result.unsupported_modulator_records = modulator_result.unsupported_records;
    if (modulator_status != ZoneModulatorResolveStatus::Ok ||
        modulator_result.zone_count != required_zones) {
        result.status = PlaybackPrepareStatus::ModulatorResolveFailed;
        return result;
    }

    const std::size_t required_dependencies = uniqueDependencyCount(
        request.native_scratch,
        required_zones);
    result.required_dependency_capacity = required_dependencies;
    result.dependency_count = required_dependencies;
    if (required_dependencies > request.dependency_capacity ||
        (required_dependencies != 0 && request.dependencies == nullptr)) {
        result.status = PlaybackPrepareStatus::DependencyCapacityExceeded;
        return result;
    }

    // First pass is deliberately side-effect free for long-lived playback
    // storage. Reject the whole program rather than dropping one sample-playing
    // zone, which would silently change the preset's key/velocity behavior.
    for (std::size_t index = 0; index < required_zones; ++index) {
        gm::PlaybackZone candidate{};
        const PlaybackMaterializationResult materialized = materializer_.materialize(
            request.native_scratch[index],
            request.modulation_scratch[index],
            &candidate);
        result.issue_union |= materialized.issues;

        if (materialized.status == PlaybackMaterializationStatus::Unrepresentable) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::UnrepresentableZone;
            return result;
        }
        if (materialized.status == PlaybackMaterializationStatus::InvalidArgument) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }
        if (materialized.status == PlaybackMaterializationStatus::Preview) {
            ++result.preview_zone_count;
            if (request.policy == PlaybackPreparePolicy::StrictQualified) {
                result.first_problem_zone = index;
                result.status = PlaybackPrepareStatus::UnqualifiedZone;
                return result;
            }
        }
    }

    // The materializer is pure/deterministic, so after preflight this second
    // pass cannot encounter a new semantic failure without a programming bug.
    for (std::size_t index = 0; index < required_zones; ++index) {
        const PlaybackMaterializationResult materialized = materializer_.materialize(
            request.native_scratch[index],
            request.modulation_scratch[index],
            &request.playback_zones[index]);
        if (!materialized.playablePreview()) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }
    }

    if (required_dependencies != 0) {
        writeUniqueDependencies(
            request.native_scratch,
            required_zones,
            request.dependencies);
    }

    gm::PlaybackProgramSnapshot candidate{};
    candidate.channel = request.channel;
    candidate.bank = request.bank;
    candidate.program = request.program;
    candidate.token = request.token;
    candidate.bank_generation = request.bank_generation;
    candidate.zones = request.playback_zones;
    candidate.zone_count = static_cast<std::uint16_t>(required_zones);
    if (!candidate.valid()) {
        result.status = PlaybackPrepareStatus::SnapshotInvalid;
        return result;
    }

    *request.snapshot = candidate;
    result.status = PlaybackPrepareStatus::Ok;
    return result;
}

}  // namespace gm_synth::sf2
