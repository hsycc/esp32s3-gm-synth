#include "gm/PlaybackProgramSnapshot.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackMaterializer.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "storage/RandomAccessReader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::sample::SampleId;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueAddressOutsideResidentSample;
using gm_synth::sf2::PlaybackIssueCustomModulatorsPending;
using gm_synth::sf2::PlaybackIssueFilterPending;
using gm_synth::sf2::PlaybackIssueGainAboveUnityClamped;
using gm_synth::sf2::PlaybackIssueIgnoredInvalidPresetGenerator;
using gm_synth::sf2::PlaybackIssueInvalidFixedVelocity;
using gm_synth::sf2::PlaybackIssueInvalidLoop;
using gm_synth::sf2::PlaybackIssueInvalidPhaseIncrement;
using gm_synth::sf2::PlaybackIssueInvalidRootKey;
using gm_synth::sf2::PlaybackIssueInvalidSampleAddress;
using gm_synth::sf2::PlaybackIssuePanLawApproximation;
using gm_synth::sf2::PlaybackIssueRomSample;
using gm_synth::sf2::PlaybackIssueScaleTuningPending;
using gm_synth::sf2::PlaybackIssueStereoOrLinkedSample;
using gm_synth::sf2::PlaybackIssueUnknownGenerator;
using gm_synth::sf2::PlaybackIssueUnsupportedSampleMode;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::PlaybackPrepareStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::storage::RandomAccessReader;

class FileReader final : public RandomAccessReader {
public:
    explicit FileReader(const char* path) : file_(path, std::ios::binary) {
        if (!file_) {
            return;
        }
        file_.seekg(0, std::ios::end);
        const std::streamoff end = file_.tellg();
        if (end >= 0) {
            size_ = static_cast<std::uint64_t>(end);
        }
        file_.clear();
    }

    bool valid() const { return file_.is_open() && size_ != 0; }
    std::uint64_t size() const override { return size_; }

    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        if (!valid() || destination == nullptr || offset > size_ || bytes > size_ - offset) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) {
            return false;
        }
        file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
        return file_.gcount() == static_cast<std::streamsize>(bytes);
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

struct IssueDef {
    std::uint64_t bit;
    const char* name;
};

constexpr std::array<IssueDef, 16> kIssueDefs{{
    {PlaybackIssueCustomModulatorsPending, "custom_modulators_pending"},
    {PlaybackIssueUnknownGenerator, "unknown_generator"},
    {PlaybackIssueInvalidSampleAddress, "invalid_sample_address"},
    {PlaybackIssueAddressOutsideResidentSample, "address_outside_resident_sample"},
    {PlaybackIssueInvalidLoop, "invalid_loop"},
    {PlaybackIssueUnsupportedSampleMode, "unsupported_sample_mode"},
    {PlaybackIssueRomSample, "rom_sample"},
    {PlaybackIssueStereoOrLinkedSample, "stereo_or_linked_sample"},
    {PlaybackIssueInvalidRootKey, "invalid_root_key"},
    {PlaybackIssueInvalidPhaseIncrement, "invalid_phase_increment"},
    {PlaybackIssueScaleTuningPending, "scale_tuning_pending"},
    {PlaybackIssueFilterPending, "filter_pending"},
    {PlaybackIssuePanLawApproximation, "pan_law_approximation"},
    {PlaybackIssueGainAboveUnityClamped, "gain_above_unity_clamped"},
    {PlaybackIssueIgnoredInvalidPresetGenerator, "ignored_invalid_preset_generator"},
    {PlaybackIssueInvalidFixedVelocity, "invalid_fixed_velocity"},
}};

const char* statusName(PlaybackPrepareStatus status) {
    switch (status) {
        case PlaybackPrepareStatus::Ok: return "ok";
        case PlaybackPrepareStatus::InvalidArgument: return "invalid_argument";
        case PlaybackPrepareStatus::ResolveFailed: return "resolve_failed";
        case PlaybackPrepareStatus::ModulatorResolveFailed: return "modulator_resolve_failed";
        case PlaybackPrepareStatus::ZoneCapacityExceeded: return "zone_capacity_exceeded";
        case PlaybackPrepareStatus::KeyIndexCapacityExceeded: return "key_index_capacity_exceeded";
        case PlaybackPrepareStatus::DependencyCapacityExceeded: return "dependency_capacity_exceeded";
        case PlaybackPrepareStatus::UnrepresentableZone: return "unrepresentable_zone";
        case PlaybackPrepareStatus::UnqualifiedZone: return "unqualified_zone";
        case PlaybackPrepareStatus::MaterializationFailed: return "materialization_failed";
        case PlaybackPrepareStatus::SnapshotInvalid: return "snapshot_invalid";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_sf2_issue_diagnostics <sf2> <asset_id>\n";
        return 2;
    }

    FileReader reader(argv[1]);
    const std::string asset = argv[2];
    if (!reader.valid()) {
        std::cerr << "M7_ISSUE_FATAL asset=" << asset << " reason=asset_not_readable\n";
        return 2;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_ISSUE_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 2;
    }

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount>
        key_spans{};
    std::vector<std::uint16_t> key_zone_indices(
        PlaybackProgramSnapshot::kMaxZones * PlaybackProgramSnapshot::kMidiKeyCount);
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer(44100);

    constexpr std::size_t kPrepareStatusCount =
        static_cast<std::size_t>(PlaybackPrepareStatus::SnapshotInvalid) + 1u;
    std::array<std::size_t, kPrepareStatusCount> status_counts{};
    std::array<std::size_t, kIssueDefs.size()> issue_counts{};
    std::size_t rejected = 0;
    std::size_t accepted = 0;
    std::size_t max_required_native = 0;
    std::size_t max_required_playback = 0;
    std::size_t max_required_key_index = 0;
    std::size_t max_required_dependencies = 0;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset) || preset.program > 255u) {
            continue;
        }

        PlaybackProgramSnapshot snapshot{};
        PlaybackPrepareRequest request{};
        request.channel = preset.bank == 128u ? 9u : 0u;
        request.bank = preset.bank;
        request.program = static_cast<std::uint8_t>(preset.program);
        request.token = static_cast<std::uint32_t>(preset_index + 1u);
        request.bank_generation = 1;
        request.policy = PlaybackPreparePolicy::StrictQualified;
        request.native_scratch = native.data();
        request.native_capacity = native.size();
        request.modulation_scratch = modulation.data();
        request.modulation_capacity = modulation.size();
        request.playback_zones = playback.data();
        request.playback_capacity = playback.size();
        request.key_spans = key_spans.data();
        request.key_zone_indices = key_zone_indices.data();
        request.key_zone_index_capacity = key_zone_indices.size();
        request.dependencies = dependencies.data();
        request.dependency_capacity = dependencies.size();
        request.snapshot = &snapshot;

        const auto result = preparer.prepare(pdta, request);
        const std::size_t status_index = static_cast<std::size_t>(result.status);
        if (status_index < status_counts.size()) {
            ++status_counts[status_index];
        }
        max_required_native = std::max(max_required_native, result.required_native_capacity);
        max_required_playback = std::max(max_required_playback, result.required_zone_capacity);
        max_required_key_index = std::max(
            max_required_key_index,
            result.required_key_index_capacity);
        max_required_dependencies = std::max(
            max_required_dependencies,
            result.required_dependency_capacity);

        if (result.ok() && snapshot.valid()) {
            ++accepted;
            continue;
        }

        ++rejected;
        for (std::size_t issue_index = 0; issue_index < kIssueDefs.size(); ++issue_index) {
            if ((result.issue_union & kIssueDefs[issue_index].bit) != 0) {
                ++issue_counts[issue_index];
            }
        }

        std::cout << "M7_ISSUE_PROGRAM"
                  << " asset=" << asset
                  << " bank=" << preset.bank
                  << " program=" << preset.program
                  << " status=" << statusName(result.status)
                  << " status_code=" << static_cast<unsigned>(result.status)
                  << " first_problem_zone=" << result.first_problem_zone
                  << " issue_union=0x" << std::hex << result.issue_union << std::dec
                  << " native_zones=" << result.native_zone_count
                  << " required_native_capacity=" << result.required_native_capacity
                  << " required_playback_capacity=" << result.required_zone_capacity
                  << " required_key_index_capacity=" << result.required_key_index_capacity
                  << " required_dependency_capacity=" << result.required_dependency_capacity
                  << " stereo_pairs=" << result.stereo_pair_count
                  << " supported_modulators=" << result.supported_modulator_records
                  << " unsupported_modulators=" << result.unsupported_modulator_records
                  << " preview_zones=" << result.preview_zone_count
                  << '\n';
    }

    for (std::size_t index = 0; index < kIssueDefs.size(); ++index) {
        std::cout << "M7_ISSUE_COUNT"
                  << " asset=" << asset
                  << " issue=" << kIssueDefs[index].name
                  << " bit=0x" << std::hex << kIssueDefs[index].bit << std::dec
                  << " rejected_programs=" << issue_counts[index]
                  << '\n';
    }

    std::cout << "M7_ISSUE_SUMMARY"
              << " asset=" << asset
              << " presets=" << pdta.presetCount()
              << " accepted=" << accepted
              << " rejected=" << rejected
              << " status_zone_capacity="
              << status_counts[static_cast<std::size_t>(PlaybackPrepareStatus::ZoneCapacityExceeded)]
              << " status_key_index_capacity="
              << status_counts[static_cast<std::size_t>(PlaybackPrepareStatus::KeyIndexCapacityExceeded)]
              << " status_unrepresentable="
              << status_counts[static_cast<std::size_t>(PlaybackPrepareStatus::UnrepresentableZone)]
              << " status_unqualified="
              << status_counts[static_cast<std::size_t>(PlaybackPrepareStatus::UnqualifiedZone)]
              << " max_required_native=" << max_required_native
              << " max_required_playback=" << max_required_playback
              << " max_required_key_index=" << max_required_key_index
              << " max_required_dependencies=" << max_required_dependencies
              << '\n';

    return 0;
}
