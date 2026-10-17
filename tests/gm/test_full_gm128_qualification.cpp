#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/sample/SampleResidencyManager.h"
#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/sf2/Sf2PlaybackProgramPreparer.h"
#include "../../firmware/sf2/Sf2SampleSource.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <tuple>

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

struct ProgramIdentity {
    std::uint16_t bank = 0;
    std::uint16_t program = 0;
};

struct MaxMetric {
    std::size_t value = 0;
    ProgramIdentity identity{};

    void observe(std::size_t candidate, ProgramIdentity program_identity) {
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
    std::size_t total_native_zones = 0;
    std::size_t total_playback_zones = 0;
    std::size_t total_stereo_pairs = 0;
    std::size_t total_dependencies = 0;
    std::uint64_t supported_modulator_records = 0;
    std::uint64_t unsupported_modulator_records = 0;
    std::size_t preview_zones = 0;
    std::size_t unique_dependency_count = 0;
    std::uint64_t unique_dependency_pcm_bytes = 0;
    std::uint64_t unique_dependency_aligned_bytes = 0;
    MaxMetric max_native_zones{};
    MaxMetric max_playback_zones{};
    MaxMetric max_dependencies{};
    MaxMetric max_pcm_working_set{};
    MaxMetric max_aligned_pcm_working_set{};
    MaxMetric max_prepare_storage_bytes{};
};

using ModulatorIdentity =
    std::tuple<std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t>;

std::size_t alignedPcmBytes(std::size_t bytes) {
    constexpr std::size_t alignment = SampleResidencyManager::kAlignment;
    static_assert((alignment & (alignment - 1u)) == 0u, "alignment must be power of two");
    return (bytes + alignment - 1u) & ~(alignment - 1u);
}

std::size_t prepareStorageBytes(const gm_synth::sf2::PlaybackPrepareResult& result) {
    // Modulation scratch is one entry per native resolved zone. Playback zones
    // and dependency storage are caller-owned publication storage, not realtime
    // heap. Reporting the combined requirement makes capacity regressions visible.
    return result.required_native_capacity * sizeof(NativeResolvedZone) +
           result.required_native_capacity * sizeof(ResolvedZoneModulation) +
           result.required_zone_capacity * sizeof(PlaybackZone) +
           result.required_dependency_capacity * sizeof(SampleId) +
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

void assertStandardDrumCoverage(const PlaybackProgramSnapshot& snapshot) {
    assert(snapshot.bank == kPercussionBank);
    assert(snapshot.program == 0);
    assert(snapshot.channel == kPercussionChannel);
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
        assert(covered);
        assert(fixed_pitch);
    }
}

std::set<ModulatorIdentity> collectNonZeroModulatorIdentities(
    Sf2PdtaReader& pdta,
    bool preset_scope,
    std::size_t* nonzero_record_count) {
    std::set<ModulatorIdentity> identities;
    *nonzero_record_count = 0;
    const std::size_t count = preset_scope
        ? pdta.presetModulatorRecordCount()
        : pdta.instrumentModulatorRecordCount();
    for (std::size_t index = 0; index < count; ++index) {
        Sf2PdtaReader::Modulator modulator{};
        const bool ok = preset_scope
            ? pdta.presetModulator(index, &modulator)
            : pdta.instrumentModulator(index, &modulator);
        assert(ok);
        if (modulator.source_oper == 0 && modulator.destination_oper == 0 &&
            modulator.amount == 0 && modulator.amount_source_oper == 0 &&
            modulator.transform_oper == 0) {
            continue;
        }
        ++(*nonzero_record_count);
        identities.emplace(
            modulator.source_oper,
            modulator.destination_oper,
            modulator.amount_source_oper,
            modulator.transform_oper);
    }
    return identities;
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

}  // namespace

int main() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));
    assert(pdta.sampleCount() == sample_source.sampleCount());
    assert(sample_source.sampleCount() <= Sf2SampleSource::kMaxSamples);

    std::array<bool, 128> melodic_programs{};
    std::array<bool, 128> percussion_programs{};
    std::array<bool, Sf2SampleSource::kMaxSamples> unique_dependencies{};

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer(44100);

    QualificationStats stats{};
    stats.preset_count = pdta.presetCount();
    constexpr std::array<std::uint8_t, 3> representative_keys{{0, 60, 127}};
    constexpr std::array<std::uint8_t, 3> representative_velocities{{1, 64, 127}};

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        assert(pdta.presetHeader(preset_index, &preset));
        assert(preset.program < 128);
        assert(preset.bank == kMelodicBank || preset.bank == kPercussionBank);

        if (preset.bank == kMelodicBank) {
            assert(!melodic_programs[preset.program]);
            melodic_programs[preset.program] = true;
            ++stats.melodic_preset_count;
        } else {
            assert(!percussion_programs[preset.program]);
            percussion_programs[preset.program] = true;
            ++stats.percussion_preset_count;
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

        const auto result = preparer.prepare(pdta, request);
        assert(result.ok());
        assert(result.native_zone_count > 0);
        assert(result.zone_count > 0);
        assert(result.zone_count <= PlaybackProgramSnapshot::kMaxZones);
        assert(result.dependency_count > 0);
        assert(result.unsupported_modulator_records == 0);
        assert(result.preview_zone_count == 0);
        assert(snapshot.valid());
        assert(snapshot.bank == preset.bank);
        assert(snapshot.program == preset.program);
        assert(snapshot.zone_count == result.zone_count);

        const ProgramIdentity identity{preset.bank, preset.program};
        stats.total_native_zones += result.native_zone_count;
        stats.total_playback_zones += result.zone_count;
        stats.total_stereo_pairs += result.stereo_pair_count;
        stats.total_dependencies += result.dependency_count;
        stats.supported_modulator_records += result.supported_modulator_records;
        stats.unsupported_modulator_records += result.unsupported_modulator_records;
        stats.preview_zones += result.preview_zone_count;
        stats.max_native_zones.observe(result.required_native_capacity, identity);
        stats.max_playback_zones.observe(result.required_zone_capacity, identity);
        stats.max_dependencies.observe(result.required_dependency_capacity, identity);
        stats.max_prepare_storage_bytes.observe(prepareStorageBytes(result), identity);

        std::size_t pcm_working_set = 0;
        std::size_t aligned_pcm_working_set = 0;
        for (std::size_t dependency_index = 0;
             dependency_index < result.dependency_count;
             ++dependency_index) {
            const SampleId sample_id = dependencies[dependency_index];
            assert(sample_id < sample_source.sampleCount());
            SampleDescriptor descriptor{};
            assert(sample_source.descriptorFor(sample_id, &descriptor));
            assert(descriptor.id == sample_id);
            assert(descriptor.pcm_bytes > 0);
            pcm_working_set += descriptor.pcm_bytes;
            aligned_pcm_working_set += alignedPcmBytes(descriptor.pcm_bytes);
            unique_dependencies[sample_id] = true;
        }
        stats.max_pcm_working_set.observe(pcm_working_set, identity);
        stats.max_aligned_pcm_working_set.observe(aligned_pcm_working_set, identity);

        if (preset.bank == kMelodicBank) {
            for (const std::uint8_t key : representative_keys) {
                for (const std::uint8_t velocity : representative_velocities) {
                    assert(anyZoneMatches(snapshot, key, velocity));
                }
            }
        } else if (preset.program == 0) {
            assertStandardDrumCoverage(snapshot);
        }
    }

    assert(stats.melodic_preset_count == 128);
    for (const bool present : melodic_programs) {
        assert(present);
    }
    assert(stats.percussion_preset_count >= 1);
    assert(percussion_programs[0]);
    assert(stats.unsupported_modulator_records == 0);
    assert(stats.preview_zones == 0);

    for (std::size_t sample_id = 0; sample_id < unique_dependencies.size(); ++sample_id) {
        if (!unique_dependencies[sample_id]) {
            continue;
        }
        SampleDescriptor descriptor{};
        assert(sample_source.descriptorFor(static_cast<SampleId>(sample_id), &descriptor));
        ++stats.unique_dependency_count;
        stats.unique_dependency_pcm_bytes += descriptor.pcm_bytes;
        stats.unique_dependency_aligned_bytes += alignedPcmBytes(descriptor.pcm_bytes);
    }

    std::size_t pmod_records = 0;
    std::size_t imod_records = 0;
    const auto pmod_identities =
        collectNonZeroModulatorIdentities(pdta, true, &pmod_records);
    const auto imod_identities =
        collectNonZeroModulatorIdentities(pdta, false, &imod_records);

    std::cout << "GM128_QUALIFICATION"
              << " file_bytes=" << reader.size()
              << " presets=" << stats.preset_count
              << " melodic_presets=" << stats.melodic_preset_count
              << " percussion_presets=" << stats.percussion_preset_count
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

    std::cout << "GM128_PERCUSSION_PROGRAMS";
    for (std::size_t program = 0; program < percussion_programs.size(); ++program) {
        if (percussion_programs[program]) {
            std::cout << ' ' << program;
        }
    }
    std::cout << '\n';

    printMaxMetric("native_zones", stats.max_native_zones);
    printMaxMetric("playback_zones", stats.max_playback_zones);
    printMaxMetric("sample_dependencies", stats.max_dependencies);
    printMaxMetric("pcm_working_set_bytes", stats.max_pcm_working_set);
    printMaxMetric("aligned_pcm_working_set_bytes", stats.max_aligned_pcm_working_set);
    printMaxMetric("prepare_storage_bytes", stats.max_prepare_storage_bytes);
    printModulatorIdentities("pmod", pmod_identities, pmod_records);
    printModulatorIdentities("imod", imod_identities, imod_records);

    std::cout << "Full FM GM128 qualification checks passed\n";
    return 0;
}
