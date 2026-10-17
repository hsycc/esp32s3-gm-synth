#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/sample/SampleResidencyManager.h"
#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/sf2/Sf2PlaybackProgramPreparer.h"
#include "../../firmware/sf2/Sf2SampleSource.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::PlaybackPrepareResult;
using gm_synth::sf2::PlaybackPrepareStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::Sf2SampleSource;
using gm_synth::storage::RandomAccessReader;

constexpr const char* kFullBankPath =
    "assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2";
constexpr std::uint16_t kMelodicBank = 0;
constexpr std::uint16_t kPercussionBank = 128;
constexpr std::uint8_t kPercussionChannel = 9;

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

struct ProgramIdentity {
    std::uint16_t bank = 0;
    std::uint16_t program = 0;
};

struct MaxMetric {
    std::uint64_t value = 0;
    ProgramIdentity identity{};

    void observe(std::uint64_t candidate, ProgramIdentity program_identity) {
        if (candidate > value) {
            value = candidate;
            identity = program_identity;
        }
    }
};

struct QualificationStats {
    std::size_t preset_count = 0;
    std::size_t melodic_preset_count = 0;
    std::size_t percussion_preset_count = 0;
    std::size_t other_bank_preset_count = 0;
    std::size_t prepare_failures = 0;
    std::size_t metadata_failures = 0;
    std::size_t duplicate_identities = 0;
    std::size_t representative_grid_misses = 0;
    std::size_t mandatory_coverage_misses = 0;
    std::size_t drum_coverage_misses = 0;
    std::size_t dependency_failures = 0;
    std::uint64_t total_native_zones = 0;
    std::uint64_t total_playback_zones = 0;
    std::uint64_t total_stereo_pairs = 0;
    std::uint64_t total_dependencies = 0;
    std::uint64_t supported_modulator_records = 0;
    std::uint64_t unsupported_modulator_records = 0;
    std::uint64_t preview_zones = 0;
    std::size_t unique_dependency_count = 0;
    std::uint64_t unique_dependency_pcm_bytes = 0;
    std::uint64_t unique_dependency_aligned_bytes = 0;
    MaxMetric max_native_zones{};
    MaxMetric max_playback_zones{};
    MaxMetric max_stereo_pairs{};
    MaxMetric max_dependencies{};
    MaxMetric max_pcm_working_set{};
    MaxMetric max_aligned_pcm_working_set{};
    MaxMetric max_prepare_storage_bytes{};
};

using ModulatorIdentity =
    std::tuple<std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t>;

const char* statusName(PlaybackPrepareStatus status) {
    switch (status) {
        case PlaybackPrepareStatus::Ok: return "Ok";
        case PlaybackPrepareStatus::InvalidArgument: return "InvalidArgument";
        case PlaybackPrepareStatus::ResolveFailed: return "ResolveFailed";
        case PlaybackPrepareStatus::ModulatorResolveFailed: return "ModulatorResolveFailed";
        case PlaybackPrepareStatus::ZoneCapacityExceeded: return "ZoneCapacityExceeded";
        case PlaybackPrepareStatus::DependencyCapacityExceeded: return "DependencyCapacityExceeded";
        case PlaybackPrepareStatus::UnrepresentableZone: return "UnrepresentableZone";
        case PlaybackPrepareStatus::UnqualifiedZone: return "UnqualifiedZone";
        case PlaybackPrepareStatus::MaterializationFailed: return "MaterializationFailed";
        case PlaybackPrepareStatus::SnapshotInvalid: return "SnapshotInvalid";
    }
    return "Unknown";
}

std::uint64_t alignedPcmBytes(std::uint64_t bytes) {
    constexpr std::uint64_t alignment = SampleResidencyManager::kAlignment;
    static_assert((alignment & (alignment - 1u)) == 0u, "alignment must be power of two");
    return (bytes + alignment - 1u) & ~(alignment - 1u);
}

std::uint64_t prepareStorageBytes(const PlaybackPrepareResult& result) {
    return static_cast<std::uint64_t>(result.required_native_capacity) *
               sizeof(NativeResolvedZone) +
           static_cast<std::uint64_t>(result.required_native_capacity) *
               sizeof(ResolvedZoneModulation) +
           static_cast<std::uint64_t>(result.required_zone_capacity) *
               sizeof(PlaybackZone) +
           static_cast<std::uint64_t>(result.required_dependency_capacity) *
               sizeof(SampleId) +
           sizeof(PlaybackProgramSnapshot);
}

bool anyZoneMatches(
    const PlaybackProgramSnapshot& snapshot,
    std::uint8_t key,
    std::uint8_t velocity) {
    for (std::size_t index = 0; index < snapshot.zone_count; ++index) {
        if (snapshot.zones[index].matches(key, velocity)) {
            return true;
        }
    }
    return false;
}

std::size_t countRepresentativeGridMisses(const PlaybackProgramSnapshot& snapshot) {
    constexpr std::array<std::uint8_t, 3> keys{{0, 60, 127}};
    constexpr std::array<std::uint8_t, 3> velocities{{1, 64, 127}};
    std::size_t misses = 0;
    for (const std::uint8_t key : keys) {
        for (const std::uint8_t velocity : velocities) {
            if (!anyZoneMatches(snapshot, key, velocity)) {
                ++misses;
            }
        }
    }
    return misses;
}

std::size_t countMandatoryMelodicCoverageMisses(
    const PlaybackProgramSnapshot& snapshot) {
    constexpr std::array<std::uint8_t, 3> keys{{0, 60, 127}};
    std::size_t misses = 0;
    for (const std::uint8_t key : keys) {
        if (!anyZoneMatches(snapshot, key, 96)) {
            ++misses;
        }
    }
    return misses;
}

std::size_t countStandardDrumCoverageMisses(
    const PlaybackProgramSnapshot& snapshot) {
    std::size_t misses = 0;
    for (std::uint16_t key = 35; key <= 81; ++key) {
        bool covered = false;
        bool fixed_pitch = false;
        for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
            const PlaybackZone& zone = snapshot.zones[zone_index];
            if (key >= zone.key_low && key <= zone.key_high &&
                96 >= zone.velocity_low && 96 <= zone.velocity_high) {
                covered = true;
                fixed_pitch = fixed_pitch ||
                    (!zone.key_tracking && zone.scale_tuning_cents_per_key == 0);
            }
        }
        if (!covered || !fixed_pitch) {
            ++misses;
        }
    }
    return misses;
}

bool collectNonZeroModulatorIdentities(
    Sf2PdtaReader& pdta,
    bool preset_scope,
    std::set<ModulatorIdentity>* identities,
    std::size_t* nonzero_record_count) {
    if (identities == nullptr || nonzero_record_count == nullptr) {
        return false;
    }
    identities->clear();
    *nonzero_record_count = 0;
    const std::size_t count = preset_scope
        ? pdta.presetModulatorRecordCount()
        : pdta.instrumentModulatorRecordCount();
    for (std::size_t index = 0; index < count; ++index) {
        Sf2PdtaReader::Modulator modulator{};
        const bool ok = preset_scope
            ? pdta.presetModulator(index, &modulator)
            : pdta.instrumentModulator(index, &modulator);
        if (!ok) {
            return false;
        }
        if (modulator.source_oper == 0 && modulator.destination_oper == 0 &&
            modulator.amount == 0 && modulator.amount_source_oper == 0 &&
            modulator.transform_oper == 0) {
            continue;
        }
        ++(*nonzero_record_count);
        identities->emplace(
            modulator.source_oper,
            modulator.destination_oper,
            modulator.amount_source_oper,
            modulator.transform_oper);
    }
    return true;
}

void printModulatorIdentities(
    const char* scope,
    const std::set<ModulatorIdentity>& identities,
    std::size_t record_count) {
    std::cout << "GM128_MODULATORS scope=" << scope
              << " nonzero_records=" << record_count
              << " unique_identities=" << identities.size() << '\n';
    for (const auto& identity : identities) {
        std::cout << "GM128_MOD_ID scope=" << scope
                  << " src=0x" << std::hex << std::uppercase << std::setw(4)
                  << std::setfill('0') << std::get<0>(identity)
                  << " dest=0x" << std::setw(4) << std::get<1>(identity)
                  << " amount_src=0x" << std::setw(4) << std::get<2>(identity)
                  << " transform=0x" << std::setw(4) << std::get<3>(identity)
                  << std::dec << std::nouppercase << std::setfill(' ') << '\n';
    }
}

void printMaxMetric(const char* name, const MaxMetric& metric) {
    std::cout << "GM128_MAX metric=" << name
              << " value=" << metric.value
              << " bank=" << metric.identity.bank
              << " program=" << metric.identity.program << '\n';
}

void printBankPrograms(
    const std::map<std::uint16_t, std::vector<std::uint16_t>>& bank_programs) {
    for (const auto& entry : bank_programs) {
        std::cout << "GM128_BANK bank=" << entry.first
                  << " presets=" << entry.second.size()
                  << " programs=";
        bool first = true;
        for (const std::uint16_t program : entry.second) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << program;
        }
        std::cout << '\n';
    }
}

}  // namespace

int main() {
    FileReader reader(kFullBankPath);
    if (!reader.valid()) {
        std::cerr << "GM128_FAILURE reason=full_bank_not_readable path=" << kFullBankPath << '\n';
        return 1;
    }

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    if (!pdta.begin(reader)) {
        std::cerr << "GM128_FAILURE reason=pdta_begin error="
                  << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 1;
    }
    if (!sample_source.begin(reader)) {
        std::cerr << "GM128_FAILURE reason=sample_source_begin error="
                  << static_cast<unsigned>(sample_source.lastError()) << '\n';
        return 1;
    }
    if (pdta.sampleCount() != sample_source.sampleCount() ||
        sample_source.sampleCount() > Sf2SampleSource::kMaxSamples) {
        std::cerr << "GM128_FAILURE reason=sample_count_mismatch pdta=" << pdta.sampleCount()
                  << " source=" << sample_source.sampleCount()
                  << " max=" << Sf2SampleSource::kMaxSamples << '\n';
        return 1;
    }

    std::array<bool, 128> melodic_programs{};
    std::array<bool, 128> percussion_programs{};
    std::array<bool, Sf2SampleSource::kMaxSamples> unique_dependencies{};
    std::set<std::pair<std::uint16_t, std::uint16_t>> preset_identities;
    std::map<std::uint16_t, std::vector<std::uint16_t>> bank_programs;

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer(44100);

    QualificationStats stats{};
    stats.preset_count = pdta.presetCount();

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset)) {
            ++stats.metadata_failures;
            std::cerr << "GM128_FAILURE reason=preset_header_read index=" << preset_index << '\n';
            continue;
        }

        bank_programs[preset.bank].push_back(preset.program);
        const ProgramIdentity identity{preset.bank, preset.program};
        if (!preset_identities.emplace(preset.bank, preset.program).second) {
            ++stats.duplicate_identities;
            std::cerr << "GM128_FAILURE reason=duplicate_preset bank=" << preset.bank
                      << " program=" << preset.program << '\n';
        }

        if (preset.bank == kMelodicBank && preset.program < melodic_programs.size()) {
            melodic_programs[preset.program] = true;
            ++stats.melodic_preset_count;
        } else if (preset.bank == kPercussionBank && preset.program < percussion_programs.size()) {
            percussion_programs[preset.program] = true;
            ++stats.percussion_preset_count;
        } else if (preset.bank != kMelodicBank && preset.bank != kPercussionBank) {
            ++stats.other_bank_preset_count;
        }

        if (preset.program > 255u) {
            ++stats.prepare_failures;
            std::cerr << "GM128_FAILURE reason=program_out_of_runtime_range bank=" << preset.bank
                      << " program=" << preset.program << '\n';
            continue;
        }

        PlaybackProgramSnapshot snapshot{};
        PlaybackPrepareRequest request{};
        request.channel = preset.bank == kPercussionBank ? kPercussionChannel : 0;
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
        request.dependencies = dependencies.data();
        request.dependency_capacity = dependencies.size();
        request.snapshot = &snapshot;

        const PlaybackPrepareResult result = preparer.prepare(pdta, request);
        stats.supported_modulator_records += result.supported_modulator_records;
        stats.unsupported_modulator_records += result.unsupported_modulator_records;
        stats.preview_zones += result.preview_zone_count;
        stats.max_native_zones.observe(result.required_native_capacity, identity);
        stats.max_playback_zones.observe(result.required_zone_capacity, identity);
        stats.max_stereo_pairs.observe(result.stereo_pair_count, identity);
        stats.max_dependencies.observe(result.required_dependency_capacity, identity);
        stats.max_prepare_storage_bytes.observe(prepareStorageBytes(result), identity);

        if (!result.ok()) {
            ++stats.prepare_failures;
            std::cerr << "GM128_FAILURE reason=prepare"
                      << " bank=" << preset.bank
                      << " program=" << preset.program
                      << " status=" << statusName(result.status)
                      << " resolve_status=" << static_cast<unsigned>(result.resolve_status)
                      << " mod_status=" << static_cast<unsigned>(result.modulator_resolve_status)
                      << " native=" << result.native_zone_count
                      << " required_native=" << result.required_native_capacity
                      << " zones=" << result.zone_count
                      << " required_zones=" << result.required_zone_capacity
                      << " deps=" << result.dependency_count
                      << " required_deps=" << result.required_dependency_capacity
                      << " unsupported_modulators=" << result.unsupported_modulator_records
                      << " preview=" << result.preview_zone_count
                      << " first_problem_zone=" << result.first_problem_zone
                      << '\n';
            continue;
        }

        if (!snapshot.valid() || snapshot.bank != preset.bank ||
            snapshot.program != static_cast<std::uint8_t>(preset.program) ||
            snapshot.zone_count != result.zone_count || result.native_zone_count == 0 ||
            result.zone_count == 0 || result.zone_count > PlaybackProgramSnapshot::kMaxZones ||
            result.dependency_count == 0) {
            ++stats.metadata_failures;
            std::cerr << "GM128_FAILURE reason=prepared_snapshot_invariant"
                      << " bank=" << preset.bank
                      << " program=" << preset.program << '\n';
            continue;
        }

        stats.total_native_zones += result.native_zone_count;
        stats.total_playback_zones += result.zone_count;
        stats.total_stereo_pairs += result.stereo_pair_count;
        stats.total_dependencies += result.dependency_count;

        std::uint64_t pcm_working_set = 0;
        std::uint64_t aligned_pcm_working_set = 0;
        for (std::size_t dependency_index = 0;
             dependency_index < result.dependency_count;
             ++dependency_index) {
            const SampleId sample_id = dependencies[dependency_index];
            if (sample_id >= sample_source.sampleCount()) {
                ++stats.dependency_failures;
                std::cerr << "GM128_FAILURE reason=dependency_id_range"
                          << " bank=" << preset.bank
                          << " program=" << preset.program
                          << " sample_id=" << sample_id << '\n';
                continue;
            }
            SampleDescriptor descriptor{};
            if (!sample_source.descriptorFor(sample_id, &descriptor) ||
                descriptor.id != sample_id || descriptor.pcm_bytes == 0) {
                ++stats.dependency_failures;
                std::cerr << "GM128_FAILURE reason=dependency_descriptor"
                          << " bank=" << preset.bank
                          << " program=" << preset.program
                          << " sample_id=" << sample_id << '\n';
                continue;
            }
            pcm_working_set += descriptor.pcm_bytes;
            aligned_pcm_working_set += alignedPcmBytes(descriptor.pcm_bytes);
            unique_dependencies[sample_id] = true;
        }
        stats.max_pcm_working_set.observe(pcm_working_set, identity);
        stats.max_aligned_pcm_working_set.observe(aligned_pcm_working_set, identity);

        if (preset.bank == kMelodicBank && preset.program < 128u) {
            const std::size_t mandatory_misses =
                countMandatoryMelodicCoverageMisses(snapshot);
            const std::size_t grid_misses = countRepresentativeGridMisses(snapshot);
            stats.mandatory_coverage_misses += mandatory_misses;
            stats.representative_grid_misses += grid_misses;
            if (mandatory_misses != 0) {
                std::cerr << "GM128_FAILURE reason=melodic_key_coverage"
                          << " bank=" << preset.bank
                          << " program=" << preset.program
                          << " misses=" << mandatory_misses << '\n';
            }
            if (grid_misses != 0) {
                std::cout << "GM128_COVERAGE bank=" << preset.bank
                          << " program=" << preset.program
                          << " representative_grid_misses=" << grid_misses << '\n';
            }
        }

        if (preset.bank == kPercussionBank && preset.program == 0u) {
            const std::size_t drum_misses = countStandardDrumCoverageMisses(snapshot);
            stats.drum_coverage_misses += drum_misses;
            if (snapshot.channel != kPercussionChannel || drum_misses != 0) {
                std::cerr << "GM128_FAILURE reason=standard_drum_coverage"
                          << " bank=" << preset.bank
                          << " program=" << preset.program
                          << " channel=" << static_cast<unsigned>(snapshot.channel)
                          << " misses=" << drum_misses << '\n';
            }
        }
    }

    for (std::size_t sample_id = 0; sample_id < unique_dependencies.size(); ++sample_id) {
        if (!unique_dependencies[sample_id]) {
            continue;
        }
        SampleDescriptor descriptor{};
        if (!sample_source.descriptorFor(static_cast<SampleId>(sample_id), &descriptor)) {
            ++stats.dependency_failures;
            continue;
        }
        ++stats.unique_dependency_count;
        stats.unique_dependency_pcm_bytes += descriptor.pcm_bytes;
        stats.unique_dependency_aligned_bytes += alignedPcmBytes(descriptor.pcm_bytes);
    }

    std::size_t missing_melodic_programs = 0;
    for (const bool present : melodic_programs) {
        if (!present) {
            ++missing_melodic_programs;
        }
    }

    std::set<ModulatorIdentity> pmod_identities;
    std::set<ModulatorIdentity> imod_identities;
    std::size_t pmod_records = 0;
    std::size_t imod_records = 0;
    const bool pmod_ok = collectNonZeroModulatorIdentities(
        pdta, true, &pmod_identities, &pmod_records);
    const bool imod_ok = collectNonZeroModulatorIdentities(
        pdta, false, &imod_identities, &imod_records);
    if (!pmod_ok || !imod_ok) {
        ++stats.metadata_failures;
        std::cerr << "GM128_FAILURE reason=modulator_table_read"
                  << " pmod_ok=" << pmod_ok << " imod_ok=" << imod_ok << '\n';
    }

    std::cout << "GM128_QUALIFICATION"
              << " file_bytes=" << reader.size()
              << " presets=" << stats.preset_count
              << " banks=" << bank_programs.size()
              << " melodic_presets=" << stats.melodic_preset_count
              << " percussion_presets=" << stats.percussion_preset_count
              << " other_bank_presets=" << stats.other_bank_preset_count
              << " samples=" << sample_source.sampleCount()
              << " smpl_payload_bytes=" << pdta.smplPayloadBytes()
              << " total_native_zones=" << stats.total_native_zones
              << " total_playback_zones=" << stats.total_playback_zones
              << " stereo_pairs=" << stats.total_stereo_pairs
              << " total_dependencies=" << stats.total_dependencies
              << " unique_dependencies=" << stats.unique_dependency_count
              << " unique_pcm_bytes=" << stats.unique_dependency_pcm_bytes
              << " unique_aligned_pcm_bytes=" << stats.unique_dependency_aligned_bytes
              << " supported_modulator_records=" << stats.supported_modulator_records
              << " unsupported_modulator_records=" << stats.unsupported_modulator_records
              << " preview_zones=" << stats.preview_zones
              << '\n';

    printBankPrograms(bank_programs);
    std::cout << "GM128_COVERAGE_SUMMARY"
              << " missing_melodic_programs=" << missing_melodic_programs
              << " mandatory_melodic_misses=" << stats.mandatory_coverage_misses
              << " representative_grid_misses=" << stats.representative_grid_misses
              << " drum_key_misses=" << stats.drum_coverage_misses
              << '\n';
    std::cout << "GM128_FAILURE_SUMMARY"
              << " prepare_failures=" << stats.prepare_failures
              << " metadata_failures=" << stats.metadata_failures
              << " dependency_failures=" << stats.dependency_failures
              << " duplicate_identities=" << stats.duplicate_identities
              << '\n';

    printMaxMetric("native_zones", stats.max_native_zones);
    printMaxMetric("playback_zones", stats.max_playback_zones);
    printMaxMetric("stereo_pairs", stats.max_stereo_pairs);
    printMaxMetric("sample_dependencies", stats.max_dependencies);
    printMaxMetric("pcm_working_set_bytes", stats.max_pcm_working_set);
    printMaxMetric("aligned_pcm_working_set_bytes", stats.max_aligned_pcm_working_set);
    printMaxMetric("prepare_storage_bytes", stats.max_prepare_storage_bytes);
    printModulatorIdentities("pmod", pmod_identities, pmod_records);
    printModulatorIdentities("imod", imod_identities, imod_records);

    const bool bank0_complete = stats.melodic_preset_count == 128 &&
        missing_melodic_programs == 0;
    const bool standard_drum_present = stats.percussion_preset_count != 0 &&
        percussion_programs[0];
    const bool semantic_failures = stats.prepare_failures != 0 ||
        stats.metadata_failures != 0 || stats.dependency_failures != 0 ||
        stats.duplicate_identities != 0 || stats.mandatory_coverage_misses != 0 ||
        stats.drum_coverage_misses != 0 || stats.unsupported_modulator_records != 0 ||
        stats.preview_zones != 0;

    if (!bank0_complete || !standard_drum_present || semantic_failures) {
        std::cerr << "GM128_FAILURE reason=qualification_summary"
                  << " bank0_complete=" << bank0_complete
                  << " standard_drum_present=" << standard_drum_present
                  << " semantic_failures=" << semantic_failures << '\n';
        return 1;
    }

    std::cout << "Full FM GM128 qualification checks passed\n";
    return 0;
}
