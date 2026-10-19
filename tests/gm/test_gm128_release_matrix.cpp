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
#include <limits>

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

struct MatrixStats {
    std::size_t rows = 0;
    std::size_t melodic_programs = 0;
    std::size_t percussion_programs = 0;
    std::size_t prepare_failures = 0;
    std::size_t coverage_failures = 0;
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

}  // namespace

int main() {
    FileReader reader(kFullBankPath);
    if (!reader.valid()) {
        std::cerr << "M7_MATRIX_FAILURE reason=full_bank_not_readable path=" << kFullBankPath << '\n';
        return 1;
    }

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    if (!pdta.begin(reader) || !sample_source.begin(reader)) {
        std::cerr << "M7_MATRIX_FAILURE reason=sf2_open\n";
        return 1;
    }

    std::array<bool, 128> melodic_seen{};
    std::array<bool, 128> percussion_seen{};
    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer(44100);
    MatrixStats stats{};

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset)) {
            ++stats.prepare_failures;
            continue;
        }
        if ((preset.bank != kMelodicBank && preset.bank != kPercussionBank) ||
            preset.program >= 128u) {
            continue;
        }

        std::array<bool, 128>& seen = preset.bank == kMelodicBank
            ? melodic_seen
            : percussion_seen;
        if (seen[preset.program]) {
            ++stats.prepare_failures;
            std::cerr << "M7_MATRIX_FAILURE reason=duplicate bank=" << preset.bank
                      << " program=" << preset.program << '\n';
            continue;
        }
        seen[preset.program] = true;
        if (preset.bank == kMelodicBank) {
            ++stats.melodic_programs;
        } else {
            ++stats.percussion_programs;
        }
        ++stats.rows;

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

        const auto result = preparer.prepare(pdta, request);
        if (!result.ok() || !snapshot.valid()) {
            ++stats.prepare_failures;
            std::cout << "M7_PROGRAM bank=" << preset.bank
                      << " program=" << preset.program
                      << " class=" << (preset.bank == kMelodicBank ? "melodic" : "percussion")
                      << " status=FAIL"
                      << " zones=" << result.zone_count
                      << " deps=" << result.dependency_count
                      << " unsupported_modulators=" << result.unsupported_modulator_records
                      << " preview_zones=" << result.preview_zone_count
                      << '\n';
            continue;
        }

        std::uint64_t pcm_bytes = 0;
        std::uint64_t aligned_pcm_bytes = 0;
        bool dependency_ok = true;
        for (std::size_t index = 0; index < result.dependency_count; ++index) {
            SampleDescriptor descriptor{};
            const SampleId sample_id = dependencies[index];
            if (sample_id >= sample_source.sampleCount() ||
                !sample_source.descriptorFor(sample_id, &descriptor) ||
                descriptor.pcm_bytes == 0) {
                dependency_ok = false;
                break;
            }
            pcm_bytes += descriptor.pcm_bytes;
            aligned_pcm_bytes += alignedPcmBytes(descriptor.pcm_bytes);
        }

        std::size_t coverage_misses = 0;
        bool drum_checked = false;
        if (preset.bank == kMelodicBank) {
            coverage_misses = melodicCoverageMisses(snapshot);
        } else if (preset.program == 0u) {
            drum_checked = true;
            coverage_misses = standardDrumCoverageMisses(snapshot);
        }

        const bool row_ok = dependency_ok && coverage_misses == 0 &&
            result.unsupported_modulator_records == 0 && result.preview_zone_count == 0;
        if (!row_ok) {
            ++stats.coverage_failures;
        }
        stats.unsupported_modulators += result.unsupported_modulator_records;
        stats.preview_zones += result.preview_zone_count;

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

        std::cout << "M7_PROGRAM bank=" << preset.bank
                  << " program=" << preset.program
                  << " class=" << (preset.bank == kMelodicBank ? "melodic" : "percussion")
                  << " status=" << (row_ok ? "PASS" : "FAIL")
                  << " channel=" << static_cast<unsigned>(snapshot.channel)
                  << " native_zones=" << result.native_zone_count
                  << " zones=" << result.zone_count
                  << " deps=" << result.dependency_count
                  << " stereo_pairs=" << result.stereo_pair_count
                  << " pcm_bytes=" << pcm_bytes
                  << " aligned_pcm_bytes=" << aligned_pcm_bytes
                  << " supported_modulators=" << result.supported_modulator_records
                  << " unsupported_modulators=" << result.unsupported_modulator_records
                  << " preview_zones=" << result.preview_zone_count
                  << " coverage_misses=" << coverage_misses
                  << " drum_checked=" << (drum_checked ? 1 : 0)
                  << '\n';
    }

    std::size_t missing_melodic = 0;
    std::size_t missing_percussion = 0;
    for (std::size_t program = 0; program < 128; ++program) {
        missing_melodic += melodic_seen[program] ? 0u : 1u;
        missing_percussion += percussion_seen[program] ? 0u : 1u;
    }

    std::cout << "M7_MATRIX_SUMMARY"
              << " rows=" << stats.rows
              << " melodic_programs=" << stats.melodic_programs
              << " percussion_programs=" << stats.percussion_programs
              << " missing_melodic=" << missing_melodic
              << " missing_percussion=" << missing_percussion
              << " prepare_failures=" << stats.prepare_failures
              << " coverage_failures=" << stats.coverage_failures
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
              << '\n';

    const bool pass = stats.rows == 256 && stats.melodic_programs == 128 &&
        stats.percussion_programs == 128 && missing_melodic == 0 &&
        missing_percussion == 0 && stats.prepare_failures == 0 &&
        stats.coverage_failures == 0 && stats.unsupported_modulators == 0 &&
        stats.preview_zones == 0;
    if (!pass) {
        std::cerr << "M7_MATRIX_FAILURE reason=summary\n";
        return 1;
    }

    std::cout << "M7 release program matrix passed\n";
    return 0;
}
