#include "Sf2PlaybackProgramPreparer.h"

#include "Sf2StereoPair.h"

#include <algorithm>
#include <limits>

namespace gm_synth::sf2 {
namespace {

bool loadStereoExpansion(
    Sf2PdtaReader& pdta,
    const NativeResolvedZone& zone,
    StereoPairStatus* status,
    StereoPairExpansion* expansion) {
    if (status == nullptr || expansion == nullptr) {
        return false;
    }

    const std::uint16_t type = baseSampleType(zone.sample_type);
    if (type == kSf2MonoSampleType) {
        *status = StereoPairStatus::NotStereo;
        return true;
    }
    if (type == kSf2LinkedSampleType) {
        *status = StereoPairStatus::UnsupportedLinked;
        return true;
    }
    if (!isStereoSideSample(zone.sample_type) ||
        static_cast<std::size_t>(zone.sample_link) >= pdta.sampleCount()) {
        *status = StereoPairStatus::InvalidPair;
        return true;
    }

    Sf2PdtaReader::SampleHeader linked{};
    if (!pdta.sampleHeader(zone.sample_link, &linked)) {
        return false;
    }
    *status = expandStereoPair(
        zone,
        zone.sample_link,
        linked,
        pdta.smplPointCount(),
        expansion);
    return true;
}

bool zoneReferencesSample(const NativeResolvedZone& zone, sample::SampleId sample_id) {
    if (zone.sample_id == sample_id) {
        return true;
    }
    return isStereoSideSample(zone.sample_type) && zone.sample_link == sample_id;
}

std::size_t uniqueDependencyCount(
    const NativeResolvedZone* zones,
    std::size_t zone_count) {
    std::size_t unique = 0;
    for (std::size_t index = 0; index < zone_count; ++index) {
        const sample::SampleId candidates[2]{
            zones[index].sample_id,
            static_cast<sample::SampleId>(zones[index].sample_link),
        };
        const std::size_t candidate_count = isStereoSideSample(zones[index].sample_type) ? 2u : 1u;
        for (std::size_t candidate = 0; candidate < candidate_count; ++candidate) {
            if (candidate != 0 && candidates[candidate] == candidates[0]) {
                continue;
            }
            bool seen = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (zoneReferencesSample(zones[previous], candidates[candidate])) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                ++unique;
            }
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
        const sample::SampleId candidates[2]{
            zones[index].sample_id,
            static_cast<sample::SampleId>(zones[index].sample_link),
        };
        const std::size_t candidate_count = isStereoSideSample(zones[index].sample_type) ? 2u : 1u;
        for (std::size_t candidate = 0; candidate < candidate_count; ++candidate) {
            if (candidate != 0 && candidates[candidate] == candidates[0]) {
                continue;
            }
            bool seen = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (zoneReferencesSample(zones[previous], candidates[candidate])) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                dependencies[written++] = candidates[candidate];
            }
        }
    }
}

PlaybackPrepareStatus classifyMaterializationFailure(
    const PlaybackMaterializationResult& materialized,
    PlaybackPreparePolicy policy) {
    if (materialized.status == PlaybackMaterializationStatus::Unrepresentable) {
        return PlaybackPrepareStatus::UnrepresentableZone;
    }
    if (materialized.status == PlaybackMaterializationStatus::InvalidArgument) {
        return PlaybackPrepareStatus::MaterializationFailed;
    }
    if (materialized.status == PlaybackMaterializationStatus::Preview &&
        policy == PlaybackPreparePolicy::StrictQualified) {
        return PlaybackPrepareStatus::UnqualifiedZone;
    }
    return PlaybackPrepareStatus::Ok;
}

void accumulatePreview(
    const PlaybackMaterializationResult& materialized,
    PlaybackPrepareResult* result) {
    if (result == nullptr) {
        return;
    }
    result->issue_union |= materialized.issues;
    if (materialized.status == PlaybackMaterializationStatus::Preview) {
        ++result->preview_zone_count;
    }
}

PlaybackMaterializationResult materializeStereoSide(
    const Sf2PlaybackMaterializer& materializer,
    const NativeResolvedZone& native,
    const ResolvedZoneModulation& modulation,
    bool left_channel,
    gm::PlaybackZone* output) {
    PlaybackMaterializationResult result = materializer.materialize(native, modulation, output);
    if (result.playablePreview() && output != nullptr) {
        applyStereoBasePan(output, left_channel);
        if (!output->valid()) {
            result.status = PlaybackMaterializationStatus::InvalidArgument;
        }
    }
    return result;
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

    const std::size_t required_native_zones =
        count_status == ResolveProgramStatus::OutputTooSmall
        ? count_result.required_capacity
        : count_result.zone_count;
    result.required_native_capacity = required_native_zones;
    result.native_zone_count = required_native_zones;

    if (required_native_zones > std::numeric_limits<std::uint16_t>::max() ||
        required_native_zones > request.native_capacity ||
        required_native_zones > request.modulation_capacity ||
        (required_native_zones != 0 &&
         (request.native_scratch == nullptr || request.modulation_scratch == nullptr))) {
        result.required_zone_capacity = required_native_zones;
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
        resolve_result.zone_count != required_native_zones) {
        result.status = PlaybackPrepareStatus::ResolveFailed;
        return result;
    }

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
        modulator_result.zone_count != required_native_zones) {
        result.status = PlaybackPrepareStatus::ModulatorResolveFailed;
        return result;
    }

    std::size_t required_playback_zones = 0;
    for (std::size_t index = 0; index < required_native_zones; ++index) {
        StereoPairStatus stereo_status = StereoPairStatus::NotStereo;
        StereoPairExpansion expansion{};
        if (!loadStereoExpansion(
                pdta,
                request.native_scratch[index],
                &stereo_status,
                &expansion)) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }
        required_playback_zones += stereo_status == StereoPairStatus::Ok ? 2u : 1u;
        if (stereo_status == StereoPairStatus::Ok) {
            ++result.stereo_pair_count;
        }
    }

    result.required_zone_capacity = required_playback_zones;
    result.zone_count = required_playback_zones;
    if (required_playback_zones > gm::PlaybackProgramSnapshot::kMaxZones ||
        required_playback_zones > std::numeric_limits<std::uint16_t>::max() ||
        required_playback_zones > request.playback_capacity ||
        (required_playback_zones != 0 && request.playback_zones == nullptr)) {
        result.status = PlaybackPrepareStatus::ZoneCapacityExceeded;
        return result;
    }

    // Transactional semantic preflight. Valid left/right pairs are tested as
    // two mono runtime zones; malformed/undefined linked samples intentionally
    // fall through to the normal materializer, which keeps the historical hard
    // StereoOrLinkedSample diagnostic rather than silently dropping a channel.
    for (std::size_t index = 0; index < required_native_zones; ++index) {
        StereoPairStatus stereo_status = StereoPairStatus::NotStereo;
        StereoPairExpansion expansion{};
        if (!loadStereoExpansion(
                pdta,
                request.native_scratch[index],
                &stereo_status,
                &expansion)) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }

        if (stereo_status == StereoPairStatus::Ok) {
            gm::PlaybackZone left{};
            gm::PlaybackZone right{};
            const PlaybackMaterializationResult left_result = materializeStereoSide(
                materializer_,
                expansion.left,
                request.modulation_scratch[index],
                true,
                &left);
            const PlaybackMaterializationResult right_result = materializeStereoSide(
                materializer_,
                expansion.right,
                request.modulation_scratch[index],
                false,
                &right);
            accumulatePreview(left_result, &result);
            accumulatePreview(right_result, &result);

            const PlaybackPrepareStatus left_failure = classifyMaterializationFailure(
                left_result,
                request.policy);
            const PlaybackPrepareStatus right_failure = classifyMaterializationFailure(
                right_result,
                request.policy);
            if (left_failure != PlaybackPrepareStatus::Ok ||
                right_failure != PlaybackPrepareStatus::Ok) {
                result.first_problem_zone = index;
                result.status = left_failure != PlaybackPrepareStatus::Ok
                    ? left_failure
                    : right_failure;
                return result;
            }
            continue;
        }

        gm::PlaybackZone candidate{};
        const PlaybackMaterializationResult materialized = materializer_.materialize(
            request.native_scratch[index],
            request.modulation_scratch[index],
            &candidate);
        accumulatePreview(materialized, &result);
        const PlaybackPrepareStatus failure = classifyMaterializationFailure(
            materialized,
            request.policy);
        if (failure != PlaybackPrepareStatus::Ok) {
            result.first_problem_zone = index;
            result.status = failure;
            return result;
        }
    }

    const std::size_t required_dependencies = uniqueDependencyCount(
        request.native_scratch,
        required_native_zones);
    result.required_dependency_capacity = required_dependencies;
    result.dependency_count = required_dependencies;
    if (required_dependencies > request.dependency_capacity ||
        (required_dependencies != 0 && request.dependencies == nullptr)) {
        result.status = PlaybackPrepareStatus::DependencyCapacityExceeded;
        return result;
    }

    std::size_t playback_written = 0;
    for (std::size_t index = 0; index < required_native_zones; ++index) {
        StereoPairStatus stereo_status = StereoPairStatus::NotStereo;
        StereoPairExpansion expansion{};
        if (!loadStereoExpansion(
                pdta,
                request.native_scratch[index],
                &stereo_status,
                &expansion)) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }

        if (stereo_status == StereoPairStatus::Ok) {
            const PlaybackMaterializationResult left_result = materializeStereoSide(
                materializer_,
                expansion.left,
                request.modulation_scratch[index],
                true,
                &request.playback_zones[playback_written]);
            if (!left_result.playablePreview()) {
                result.first_problem_zone = index;
                result.status = PlaybackPrepareStatus::MaterializationFailed;
                return result;
            }
            ++playback_written;

            const PlaybackMaterializationResult right_result = materializeStereoSide(
                materializer_,
                expansion.right,
                request.modulation_scratch[index],
                false,
                &request.playback_zones[playback_written]);
            if (!right_result.playablePreview()) {
                result.first_problem_zone = index;
                result.status = PlaybackPrepareStatus::MaterializationFailed;
                return result;
            }
            ++playback_written;
            continue;
        }

        const PlaybackMaterializationResult materialized = materializer_.materialize(
            request.native_scratch[index],
            request.modulation_scratch[index],
            &request.playback_zones[playback_written]);
        if (!materialized.playablePreview()) {
            result.first_problem_zone = index;
            result.status = PlaybackPrepareStatus::MaterializationFailed;
            return result;
        }
        ++playback_written;
    }

    if (playback_written != required_playback_zones) {
        result.status = PlaybackPrepareStatus::MaterializationFailed;
        return result;
    }

    if (required_dependencies != 0) {
        writeUniqueDependencies(
            request.native_scratch,
            required_native_zones,
            request.dependencies);
    }

    gm::PlaybackProgramSnapshot candidate{};
    candidate.channel = request.channel;
    candidate.bank = request.bank;
    candidate.program = request.program;
    candidate.token = request.token;
    candidate.bank_generation = request.bank_generation;
    candidate.zones = request.playback_zones;
    candidate.zone_count = static_cast<std::uint16_t>(required_playback_zones);
    if (!candidate.valid()) {
        result.status = PlaybackPrepareStatus::SnapshotInvalid;
        return result;
    }

    *request.snapshot = candidate;
    result.status = PlaybackPrepareStatus::Ok;
    return result;
}

}  // namespace gm_synth::sf2
