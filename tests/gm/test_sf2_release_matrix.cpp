#include "gm/PlaybackProgramSnapshot.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
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
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::Sf2SampleSource;
using gm_synth::storage::RandomAccessReader;

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

enum class Profile {
    Native,
    Gm128,
};

bool parseProfile(const std::string& value, Profile* profile) {
    if (profile == nullptr) {
        return false;
    }
    if (value == "native") {
        *profile = Profile::Native;
        return true;
    }
    if (value == "gm128") {
        *profile = Profile::Gm128;
        return true;
    }
    return false;
}

const char* programClass(std::uint16_t bank) {
    if (bank == kMelodicBank) {
        return "melodic";
    }
    if (bank == kPercussionBank) {
        return "percussion";
    }
    return "auxiliary";
}

std::uint64_t alignedPcmBytes(std::uint64_t bytes) {
    constexpr std::uint64_t alignment = SampleResidencyManager::kAlignment;
    static_assert((alignment & (alignment - 1u)) == 0u, "alignment must be power of two");
    return (bytes + alignment - 1u) & ~(alignment - 1u);
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

std::size_t melodicCoverageMisses(const PlaybackProgramSnapshot& snapshot) {
    constexpr std::array<std::uint8_t, 3> keys{{0, 60, 127}};
    std::size_t misses = 0;
    for (const std::uint8_t key : keys) {
        if (!anyZoneMatches(snapshot, key, 96)) {
            ++misses;
        }
    }
    return misses;
}

std::size_t standardDrumCoverageMisses(const PlaybackProgramSnapshot& snapshot) {
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

std::size_t representativeMaxOverlap(const PlaybackProgramSnapshot& snapshot) {
    constexpr std::array<std::uint8_t, 3> velocities{{1, 64, 127}};
    std::size_t maximum = 0;
    for (std::uint16_t key = 0; key < 128; ++key) {
        for (const std::uint8_t velocity : velocities) {
            std::size_t overlap = 0;
            for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
                if (snapshot.zones[zone_index].matches(
                        static_cast<std::uint8_t>(key), velocity)) {
                    ++overlap;
                }
            }
            if (overlap > maximum) {
                maximum = overlap;
            }
        }
    }
    return maximum;
}

struct MatrixStats {
    std::size_t rows = 0;
    std::size_t metadata_failures = 0;
    std::size_t duplicate_identities = 0;
    std::size_t program_range_failures = 0;
    std::size_t prepare_failures = 0;
    std::size_t dependency_failures = 0;
    std::size_t rejected_rows = 0;
    std::uint64_t unsupported_modulators = 0;
    std::uint64_t preview_zones = 0;
    std::uint64_t max_zones = 0;
    std::uint16_t max_zones_bank = 0;
    std::uint16_t max_zones_program = 0;
    std::uint64_t max_dependencies = 0;
    std::uint16_t max_dependencies_bank = 0;
    std::uint16_t max_dependencies_program = 0;
    std::uint64_t max_aligned_pcm_bytes = 0;
    std::uint16_t max_pcm_bank = 0;
    std::uint16_t max_pcm_program = 0;
    std::uint64_t max_representative_overlap = 0;
    std::uint16_t max_overlap_bank = 0;
    std::uint16_t max_overlap_program = 0;
};

void observeMax(
    std::uint64_t value,
    std::uint16_t bank,
    std::uint16_t program,
    std::uint64_t* maximum,
    std::uint16_t* maximum_bank,
    std::uint16_t* maximum_program) {
    if (value > *maximum) {
        *maximum = value;
        *maximum_bank = bank;
        *maximum_program = program;
    }
}

void printRejectedRow(
    const std::string& asset,
    std::uint16_t bank,
    std::uint16_t program,
    const char* reason) {
    std::cout << "M7_SF2_PROGRAM"
              << " asset=" << asset
              << " bank=" << bank
              << " program=" << program
              << " class=" << programClass(bank)
              << " status=REJECTED"
              << " reason=" << reason
              << " channel=" << static_cast<unsigned>(bank == kPercussionBank ? kPercussionChannel : 0)
              << " native_zones=0 zones=0 deps=0 stereo_pairs=0"
              << " pcm_bytes=0 aligned_pcm_bytes=0"
              << " representative_overlap=0"
              << " supported_modulators=0 unsupported_modulators=0"
              << " preview_zones=0 coverage_misses=0 coverage_required=0\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr
            << "usage: test_sf2_release_matrix <sf2> <asset_id> <native|gm128> "
               "<required_accept:0|1> <source_kind>\n";
        return 2;
    }

    const char* path = argv[1];
    const std::string asset = argv[2];
    const std::string profile_name = argv[3];
    const std::string required_arg = argv[4];
    const std::string source_kind = argv[5];
    Profile profile = Profile::Native;
    if (!parseProfile(profile_name, &profile) ||
        (required_arg != "0" && required_arg != "1") || asset.empty() ||
        source_kind.empty()) {
        std::cerr << "M7_SF2_FATAL asset=" << asset << " reason=invalid_arguments\n";
        return 2;
    }
    const bool required_accept = required_arg == "1";

    FileReader reader(path);
    if (!reader.valid()) {
        std::cerr << "M7_SF2_FATAL asset=" << asset
                  << " reason=asset_not_readable path=" << path << '\n';
        return 2;
    }

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_SF2_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 2;
    }
    if (!sample_source.begin(reader)) {
        std::cerr << "M7_SF2_FATAL asset=" << asset
                  << " reason=sample_source_open error="
                  << static_cast<unsigned>(sample_source.lastError()) << '\n';
        return 2;
    }

    std::cout << "M7_SF2_ASSET"
              << " asset=" << asset
              << " profile=" << profile_name
              << " required_accept=" << (required_accept ? 1 : 0)
              << " source_kind=" << source_kind
              << " path=" << path
              << " file_bytes=" << reader.size()
              << " presets=" << pdta.presetCount()
              << " samples=" << sample_source.sampleCount()
              << '\n';

    std::array<bool, 128> bank0_seen{};
    std::array<bool, 128> bank128_seen{};
    std::set<std::pair<std::uint16_t, std::uint16_t>> identities;
    std::set<std::uint16_t> banks;
    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};
    std::vector<std::uint16_t> key_zone_indices(
        PlaybackProgramSnapshot::kMaxZones * PlaybackProgramSnapshot::kMidiKeyCount);
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer(44100);
    MatrixStats stats{};

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset)) {
            ++stats.metadata_failures;
            continue;
        }

        ++stats.rows;
        banks.insert(preset.bank);
        if (preset.bank == kMelodicBank && preset.program < bank0_seen.size()) {
            bank0_seen[preset.program] = true;
        }
        if (preset.bank == kPercussionBank && preset.program < bank128_seen.size()) {
            bank128_seen[preset.program] = true;
        }

        if (!identities.emplace(preset.bank, preset.program).second) {
            ++stats.duplicate_identities;
            ++stats.rejected_rows;
            printRejectedRow(asset, preset.bank, preset.program, "duplicate_identity");
            continue;
        }
        if (preset.program > 255u) {
            ++stats.program_range_failures;
            ++stats.rejected_rows;
            printRejectedRow(asset, preset.bank, preset.program, "program_out_of_range");
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
        request.key_spans = key_spans.data();
        request.key_zone_indices = key_zone_indices.data();
        request.key_zone_index_capacity = key_zone_indices.size();
        request.dependencies = dependencies.data();
        request.dependency_capacity = dependencies.size();
        request.snapshot = &snapshot;

        const auto result = preparer.prepare(pdta, request);
        stats.unsupported_modulators += result.unsupported_modulator_records;
        stats.preview_zones += result.preview_zone_count;
        if (!result.ok() || !snapshot.valid()) {
            ++stats.prepare_failures;
            ++stats.rejected_rows;
            std::cout << "M7_SF2_PROGRAM"
                      << " asset=" << asset
                      << " bank=" << preset.bank
                      << " program=" << preset.program
                      << " class=" << programClass(preset.bank)
                      << " status=REJECTED"
                      << " reason=prepare_status_" << static_cast<unsigned>(result.status)
                      << " channel=" << static_cast<unsigned>(request.channel)
                      << " native_zones=" << result.native_zone_count
                      << " zones=" << result.zone_count
                      << " deps=" << result.dependency_count
                      << " stereo_pairs=" << result.stereo_pair_count
                      << " pcm_bytes=0 aligned_pcm_bytes=0 representative_overlap=0"
                      << " supported_modulators=" << result.supported_modulator_records
                      << " unsupported_modulators=" << result.unsupported_modulator_records
                      << " preview_zones=" << result.preview_zone_count
                      << " coverage_misses=0 coverage_required=0\n";
            continue;
        }

        std::uint64_t pcm_bytes = 0;
        std::uint64_t aligned_pcm_bytes = 0;
        bool dependency_ok = true;
        for (std::size_t index = 0; index < result.dependency_count; ++index) {
            const SampleId sample_id = dependencies[index];
            SampleDescriptor descriptor{};
            if (sample_id >= sample_source.sampleCount() ||
                !sample_source.descriptorFor(sample_id, &descriptor) ||
                descriptor.pcm_bytes == 0) {
                dependency_ok = false;
                break;
            }
            pcm_bytes += descriptor.pcm_bytes;
            aligned_pcm_bytes += alignedPcmBytes(descriptor.pcm_bytes);
        }
        if (!dependency_ok) {
            ++stats.dependency_failures;
        }

        std::size_t coverage_misses = 0;
        bool coverage_required = false;
        if (preset.bank == kMelodicBank && preset.program < 128u) {
            coverage_misses = melodicCoverageMisses(snapshot);
            coverage_required = profile == Profile::Gm128;
        } else if (preset.bank == kPercussionBank && preset.program == 0u) {
            coverage_misses = standardDrumCoverageMisses(snapshot);
            coverage_required = profile == Profile::Gm128;
        }

        const std::size_t representative_overlap = representativeMaxOverlap(snapshot);
        const bool row_ok = dependency_ok && result.unsupported_modulator_records == 0 &&
            result.preview_zone_count == 0 &&
            (!coverage_required || coverage_misses == 0);
        const char* reason = "none";
        if (!dependency_ok) {
            reason = "invalid_dependency";
        } else if (result.unsupported_modulator_records != 0) {
            reason = "unsupported_modulator";
        } else if (result.preview_zone_count != 0) {
            reason = "preview_zone";
        } else if (coverage_required && coverage_misses != 0) {
            reason = "required_coverage";
        }
        if (!row_ok) {
            ++stats.rejected_rows;
        }

        observeMax(
            result.zone_count,
            preset.bank,
            preset.program,
            &stats.max_zones,
            &stats.max_zones_bank,
            &stats.max_zones_program);
        observeMax(
            result.dependency_count,
            preset.bank,
            preset.program,
            &stats.max_dependencies,
            &stats.max_dependencies_bank,
            &stats.max_dependencies_program);
        observeMax(
            aligned_pcm_bytes,
            preset.bank,
            preset.program,
            &stats.max_aligned_pcm_bytes,
            &stats.max_pcm_bank,
            &stats.max_pcm_program);
        observeMax(
            representative_overlap,
            preset.bank,
            preset.program,
            &stats.max_representative_overlap,
            &stats.max_overlap_bank,
            &stats.max_overlap_program);

        std::cout << "M7_SF2_PROGRAM"
                  << " asset=" << asset
                  << " bank=" << preset.bank
                  << " program=" << preset.program
                  << " class=" << programClass(preset.bank)
                  << " status=" << (row_ok ? "PASS" : "REJECTED")
                  << " reason=" << reason
                  << " channel=" << static_cast<unsigned>(snapshot.channel)
                  << " native_zones=" << result.native_zone_count
                  << " zones=" << result.zone_count
                  << " deps=" << result.dependency_count
                  << " stereo_pairs=" << result.stereo_pair_count
                  << " pcm_bytes=" << pcm_bytes
                  << " aligned_pcm_bytes=" << aligned_pcm_bytes
                  << " representative_overlap=" << representative_overlap
                  << " supported_modulators=" << result.supported_modulator_records
                  << " unsupported_modulators=" << result.unsupported_modulator_records
                  << " preview_zones=" << result.preview_zone_count
                  << " coverage_misses=" << coverage_misses
                  << " coverage_required=" << (coverage_required ? 1 : 0)
                  << '\n';
    }

    std::size_t bank0_programs = 0;
    std::size_t bank128_programs = 0;
    std::size_t missing_bank0 = 0;
    std::size_t missing_bank128 = 0;
    for (std::size_t program = 0; program < 128; ++program) {
        bank0_programs += bank0_seen[program] ? 1u : 0u;
        bank128_programs += bank128_seen[program] ? 1u : 0u;
        missing_bank0 += bank0_seen[program] ? 0u : 1u;
        missing_bank128 += bank128_seen[program] ? 0u : 1u;
    }

    const bool profile_ok = profile != Profile::Gm128 ||
        (missing_bank0 == 0 && missing_bank128 == 0);
    const bool accepted = stats.rows == pdta.presetCount() &&
        stats.metadata_failures == 0 && stats.duplicate_identities == 0 &&
        stats.program_range_failures == 0 && stats.prepare_failures == 0 &&
        stats.dependency_failures == 0 && stats.rejected_rows == 0 && profile_ok;

    std::cout << "M7_SF2_SUMMARY"
              << " asset=" << asset
              << " profile=" << profile_name
              << " classification=" << (accepted ? "ACCEPTED" : "REJECTED")
              << " required_accept=" << (required_accept ? 1 : 0)
              << " source_kind=" << source_kind
              << " file_bytes=" << reader.size()
              << " presets=" << pdta.presetCount()
              << " rows=" << stats.rows
              << " banks=" << banks.size()
              << " bank0_programs=" << bank0_programs
              << " bank128_programs=" << bank128_programs
              << " missing_bank0=" << missing_bank0
              << " missing_bank128=" << missing_bank128
              << " metadata_failures=" << stats.metadata_failures
              << " duplicate_identities=" << stats.duplicate_identities
              << " program_range_failures=" << stats.program_range_failures
              << " prepare_failures=" << stats.prepare_failures
              << " dependency_failures=" << stats.dependency_failures
              << " rejected_rows=" << stats.rejected_rows
              << " unsupported_modulators=" << stats.unsupported_modulators
              << " preview_zones=" << stats.preview_zones
              << " max_zones=" << stats.max_zones
              << " max_zones_bank=" << stats.max_zones_bank
              << " max_zones_program=" << stats.max_zones_program
              << " max_dependencies=" << stats.max_dependencies
              << " max_dependencies_bank=" << stats.max_dependencies_bank
              << " max_dependencies_program=" << stats.max_dependencies_program
              << " max_aligned_pcm_bytes=" << stats.max_aligned_pcm_bytes
              << " max_pcm_bank=" << stats.max_pcm_bank
              << " max_pcm_program=" << stats.max_pcm_program
              << " max_representative_overlap=" << stats.max_representative_overlap
              << " max_overlap_bank=" << stats.max_overlap_bank
              << " max_overlap_program=" << stats.max_overlap_program
              << '\n';

    if (required_accept && !accepted) {
        std::cerr << "M7_SF2_FATAL asset=" << asset
                  << " reason=required_asset_rejected\n";
        return 1;
    }
    return 0;
}
