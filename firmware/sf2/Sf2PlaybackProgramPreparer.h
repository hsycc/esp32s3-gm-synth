#pragma once

#include "Sf2PlaybackMaterializer.h"
#include "Sf2ResolvedZone.h"
#include "Sf2ZoneModulatorResolver.h"
#include "../sample/SampleResidencyManager.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

enum class PlaybackPreparePolicy : std::uint8_t {
    // Require every zone to be exact for the current runtime. With the current
    // engine this intentionally rejects normal SF2 banks until mandatory
    // channel-state/default modulators and the remaining DSP semantics land.
    StrictQualified = 0,
    // Produce a playable snapshot when every zone is structurally representable
    // while preserving the union of qualification issues for diagnostics.
    AllowPreview,
};

enum class PlaybackPrepareStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    ResolveFailed,
    ModulatorResolveFailed,
    ZoneCapacityExceeded,
    DependencyCapacityExceeded,
    UnrepresentableZone,
    UnqualifiedZone,
    MaterializationFailed,
    SnapshotInvalid,
};

struct PlaybackPrepareRequest {
    std::uint8_t channel = 0;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint32_t token = 0;
    std::uint32_t bank_generation = 0;
    PlaybackPreparePolicy policy = PlaybackPreparePolicy::StrictQualified;

    // Temporary control-domain workspace. These arrays may live in PSRAM and
    // may be reused immediately after prepare() returns.
    NativeResolvedZone* native_scratch = nullptr;
    std::size_t native_capacity = 0;
    ResolvedZoneModulation* modulation_scratch = nullptr;
    std::size_t modulation_capacity = 0;

    // Caller-owned immutable storage after successful publication. This array
    // must outlive PlaybackProgramSnapshot while READY/Active/Retired.
    gm::PlaybackZone* playback_zones = nullptr;
    std::size_t playback_capacity = 0;

    // Unique sample IDs required by the resolved program. The same list may be
    // passed directly to sample::ProgramPrepareController so residency and
    // playback snapshots use one source of truth.
    sample::SampleId* dependencies = nullptr;
    std::size_t dependency_capacity = 0;

    // Written only on success. Caller owns the object and its publication
    // lifetime just like PlaybackProgramSnapshotExchange requires.
    gm::PlaybackProgramSnapshot* snapshot = nullptr;
};

struct PlaybackPrepareResult {
    PlaybackPrepareStatus status = PlaybackPrepareStatus::InvalidArgument;
    ResolveProgramStatus resolve_status = ResolveProgramStatus::InvalidArgument;
    ZoneModulatorResolveStatus modulator_resolve_status =
        ZoneModulatorResolveStatus::InvalidArgument;
    std::size_t zone_count = 0;
    std::size_t required_zone_capacity = 0;
    std::size_t dependency_count = 0;
    std::size_t required_dependency_capacity = 0;
    std::size_t preview_zone_count = 0;
    std::size_t first_problem_zone = static_cast<std::size_t>(-1);
    std::uint32_t supported_modulator_records = 0;
    std::uint32_t unsupported_modulator_records = 0;
    std::uint64_t issue_union = PlaybackIssueNone;

    bool ok() const { return status == PlaybackPrepareStatus::Ok; }
};

// Control-domain bridge from a parsed SF2 bank to the immutable realtime
// PlaybackProgramSnapshot. It performs count/native/modulator resolve passes and
// a materialization preflight before mutating long-lived playback storage.
class Sf2PlaybackProgramPreparer {
public:
    explicit Sf2PlaybackProgramPreparer(std::uint32_t output_sample_rate = 44100)
        : materializer_(output_sample_rate) {}

    PlaybackPrepareResult prepare(
        Sf2PdtaReader& pdta,
        const PlaybackPrepareRequest& request) const;

private:
    Sf2PlaybackMaterializer materializer_;
};

}  // namespace gm_synth::sf2
