#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/sf2/Sf2PlaybackProgramPreparer.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramKeySpan;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::sample::SampleId;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueCustomModulatorsPending;
using gm_synth::sf2::PlaybackIssueDefaultModulatorsPending;
using gm_synth::sf2::PlaybackIssueEffectsSendPending;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::PlaybackPrepareStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::ResolveProgramStatus;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::ZoneModulatorResolveStatus;
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

PlaybackPrepareRequest requestFor(
    NativeResolvedZone* native,
    std::size_t native_capacity,
    ResolvedZoneModulation* modulation,
    std::size_t modulation_capacity,
    PlaybackZone* playback,
    std::size_t playback_capacity,
    SampleId* dependencies,
    std::size_t dependency_capacity,
    PlaybackProgramSnapshot* snapshot) {
    PlaybackPrepareRequest request{};
    request.channel = 2;
    request.bank = 0;
    request.program = 0;
    request.token = 7;
    request.bank_generation = 3;
    request.policy = PlaybackPreparePolicy::AllowPreview;
    request.native_scratch = native;
    request.native_capacity = native_capacity;
    request.modulation_scratch = modulation;
    request.modulation_capacity = modulation_capacity;
    request.playback_zones = playback;
    request.playback_capacity = playback_capacity;
    request.dependencies = dependencies;
    request.dependency_capacity = dependency_capacity;
    request.snapshot = snapshot;
    return request;
}

void assertStrictQualifiedResult(
    const gm_synth::sf2::PlaybackPrepareResult& result,
    const PlaybackProgramSnapshot& snapshot,
    std::uint16_t bank,
    std::uint8_t program) {
    assert(result.status == PlaybackPrepareStatus::Ok);
    assert(result.resolve_status == ResolveProgramStatus::Ok);
    assert(result.modulator_resolve_status == ZoneModulatorResolveStatus::Ok);
    assert(result.native_zone_count > 0);
    assert(result.zone_count > 0);
    assert(result.zone_count <= PlaybackProgramSnapshot::kMaxZones);
    assert(result.dependency_count > 0);
    assert(result.unsupported_modulator_records == 0);
    assert(result.preview_zone_count == 0);
    assert((result.issue_union & PlaybackIssueDefaultModulatorsPending) == 0);
    assert((result.issue_union & PlaybackIssueCustomModulatorsPending) == 0);
    assert((result.issue_union & PlaybackIssueEffectsSendPending) == 0);
    assert(snapshot.valid());
    assert(snapshot.bank == bank);
    assert(snapshot.program == program);
    assert(snapshot.zone_count == result.zone_count);
    for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
        assert(snapshot.zones[zone_index].valid());
    }
}

bool anyZoneMatches(
    const PlaybackProgramSnapshot& snapshot,
    std::uint8_t key,
    std::uint8_t velocity) {
    for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
        if (snapshot.zones[zone_index].matches(key, velocity)) {
            return true;
        }
    }
    return false;
}

void testFmProgramPreparesQualifiedSnapshotAndResolvedDefaultRoutes() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> native{};
    std::array<ResolvedZoneModulation, 8> modulation{};
    std::array<PlaybackZone, 8> playback{};
    std::array<SampleId, 8> dependencies{};
    std::array<PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};
    std::array<std::uint16_t, PlaybackProgramSnapshot::kMidiKeyCount> key_references{};
    PlaybackProgramSnapshot snapshot{};

    PlaybackPrepareRequest request = requestFor(
        native.data(), native.size(),
        modulation.data(), modulation.size(),
        playback.data(), playback.size(),
        dependencies.data(), dependencies.size(),
        &snapshot);
    request.key_spans = key_spans.data();
    request.key_zone_indices = key_references.data();
    request.key_zone_index_capacity = key_references.size();
    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);

    assert(result.status == PlaybackPrepareStatus::Ok);
    assert(result.resolve_status == ResolveProgramStatus::Ok);
    assert(result.modulator_resolve_status == ZoneModulatorResolveStatus::Ok);
    assert(result.zone_count == 1);
    assert(result.native_zone_count == 1);
    assert(result.stereo_pair_count == 0);
    assert(result.required_native_capacity == 1);
    assert(result.required_zone_capacity == 1);
    assert(result.required_key_index_capacity > 0);
    assert(result.required_key_index_capacity <= key_references.size());
    assert(result.max_candidates_per_key == 1);
    assert(result.dependency_count == 1);
    assert(result.required_dependency_capacity == 1);
    assert(result.supported_modulator_records == 0);
    assert(result.unsupported_modulator_records == 0);
    assert(result.preview_zone_count == 0);
    assert((result.issue_union & PlaybackIssueDefaultModulatorsPending) == 0);
    assert((result.issue_union & PlaybackIssueEffectsSendPending) == 0);
    assert(snapshot.valid());
    assert(snapshot.channel == 2);
    assert(snapshot.bank == 0);
    assert(snapshot.program == 0);
    assert(snapshot.token == 7);
    assert(snapshot.bank_generation == 3);
    assert(snapshot.zones == playback.data());
    assert(snapshot.zone_count == 1);
    assert(snapshot.key_spans == key_spans.data());
    assert(snapshot.key_zone_indices == key_references.data());
    assert(snapshot.key_reference_count == result.required_key_index_capacity);
    assert(dependencies[0] == 0);
    assert(playback[0].sample_id == 0);
    assert(playback[0].note_on_gain.enabled);
    assert(playback[0].note_on_gain.velocity_to_attenuation_cb == 960);
    assert(playback[0].note_on_gain.fixed_velocity == -1);
    assert(playback[0].note_on_filter.velocity_to_resonance_cb == 0);
    assert(playback[0].velocity_to_vol_env_attack_timecents == 0);
    assert(playback[0].vibrato_lfo.enabled);
    assert(playback[0].vibrato_lfo.channel_pressure_to_pitch_cents == 50);
    assert(playback[0].vibrato_lfo.modulation_wheel_to_pitch_cents == 50);
    assert(playback[0].valid());
}

void testStrictQualificationAcceptsEffectsCompleteProgram() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> native{};
    std::array<ResolvedZoneModulation, 8> modulation{};
    std::array<PlaybackZone, 8> playback{};
    std::array<SampleId, 8> dependencies{};
    playback[0].sample_id = 999;
    playback[0].key_low = 19;
    PlaybackProgramSnapshot snapshot{};
    snapshot.token = 1234;

    PlaybackPrepareRequest request = requestFor(
        native.data(), native.size(),
        modulation.data(), modulation.size(),
        playback.data(), playback.size(),
        dependencies.data(), dependencies.size(),
        &snapshot);
    request.policy = PlaybackPreparePolicy::StrictQualified;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assert(result.status == PlaybackPrepareStatus::Ok);
    assert(result.modulator_resolve_status == ZoneModulatorResolveStatus::Ok);
    assert(result.zone_count == 1);
    assert(result.native_zone_count == 1);
    assert(result.preview_zone_count == 0);
    assert((result.issue_union & PlaybackIssueDefaultModulatorsPending) == 0);
    assert((result.issue_union & PlaybackIssueEffectsSendPending) == 0);
    assert(playback[0].sample_id == 0);
    assert(snapshot.valid());
    assert(snapshot.token == 7);
    assert(snapshot.zone_count == 1);
}

void testCheckedInFmBaselineAllPresetsStrictlyPrepare() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    // This checked-in qualification subset currently contains 67 bank-0
    // melodic presets and no bank-128 drum preset. It remains a compact fast
    // fixture; the full tracked FM bank is qualified separately below.
    assert(pdta.presetCount() == 67);

    std::array<NativeResolvedZone, 256> native{};
    std::array<ResolvedZoneModulation, 256> modulation{};
    std::array<PlaybackZone, 256> playback{};
    std::array<SampleId, 256> dependencies{};
    Sf2PlaybackProgramPreparer preparer;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        assert(pdta.presetHeader(preset_index, &preset));
        assert(preset.bank == 0);

        PlaybackProgramSnapshot snapshot{};
        PlaybackPrepareRequest request = requestFor(
            native.data(), native.size(),
            modulation.data(), modulation.size(),
            playback.data(), playback.size(),
            dependencies.data(), dependencies.size(),
            &snapshot);
        request.bank = preset.bank;
        request.program = preset.program;
        request.token = static_cast<std::uint32_t>(preset_index + 1u);
        request.policy = PlaybackPreparePolicy::StrictQualified;

        const auto result = preparer.prepare(pdta, request);
        assert(result.status == PlaybackPrepareStatus::Ok);
        assert(result.resolve_status == ResolveProgramStatus::Ok);
        assert(result.modulator_resolve_status == ZoneModulatorResolveStatus::Ok);
        assert(result.native_zone_count == 1);
        assert(result.zone_count == 1);
        assert(result.stereo_pair_count == 0);
        assert(result.unsupported_modulator_records == 0);
        assert(result.preview_zone_count == 0);
        assert((result.issue_union & PlaybackIssueDefaultModulatorsPending) == 0);
        assert((result.issue_union & PlaybackIssueCustomModulatorsPending) == 0);
        assert((result.issue_union & PlaybackIssueEffectsSendPending) == 0);
        assert(snapshot.valid());
        assert(snapshot.bank == preset.bank);
        assert(snapshot.program == preset.program);
        assert(snapshot.zone_count == result.zone_count);
        assert(playback[0].valid());
    }
}

void testFullFmGm128MelodicProgramsStrictlyPrepare() {
    FileReader reader("assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));
    assert(pdta.presetCount() >= 129);

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    Sf2PlaybackProgramPreparer preparer;
    constexpr std::array<std::uint8_t, 3> representative_keys{{0, 60, 127}};

    for (std::uint16_t program = 0; program < 128; ++program) {
        PlaybackProgramSnapshot snapshot{};
        PlaybackPrepareRequest request = requestFor(
            native.data(), native.size(),
            modulation.data(), modulation.size(),
            playback.data(), playback.size(),
            dependencies.data(), dependencies.size(),
            &snapshot);
        request.bank = 0;
        request.program = program;
        request.token = static_cast<std::uint32_t>(program + 1u);
        request.policy = PlaybackPreparePolicy::StrictQualified;

        const auto result = preparer.prepare(pdta, request);
        assertStrictQualifiedResult(
            result,
            snapshot,
            0,
            static_cast<std::uint8_t>(program));
        for (const std::uint8_t key : representative_keys) {
            assert(anyZoneMatches(snapshot, key, 96));
        }
    }
}

void testFullFmStandardDrumKitStrictlyPreparesAndCoversGmKeys() {
    FileReader reader("assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    PlaybackProgramSnapshot snapshot{};

    PlaybackPrepareRequest request = requestFor(
        native.data(), native.size(),
        modulation.data(), modulation.size(),
        playback.data(), playback.size(),
        dependencies.data(), dependencies.size(),
        &snapshot);
    request.channel = 9;
    request.bank = 128;
    request.program = 0;
    request.token = 129;
    request.policy = PlaybackPreparePolicy::StrictQualified;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assertStrictQualifiedResult(result, snapshot, 128, 0);
    assert(snapshot.channel == 9);

    for (std::uint16_t key = 35; key <= 81; ++key) {
        bool covered = false;
        bool fixed_pitch = false;
        for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
            const PlaybackZone& zone = snapshot.zones[zone_index];
            if (key >= zone.key_low && key <= zone.key_high) {
                covered = true;
                fixed_pitch = fixed_pitch ||
                    (!zone.key_tracking && zone.scale_tuning_cents_per_key == 0);
            }
        }
        assert(covered);
        assert(fixed_pitch);
    }
}

void testGeneralUserReportsRequiredZoneCapacityBeforeResolvingScratch() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 128> native{};
    std::array<ResolvedZoneModulation, 128> modulation{};
    std::array<PlaybackZone, 128> playback{};
    std::array<SampleId, 128> dependencies{};
    PlaybackProgramSnapshot snapshot{};
    snapshot.token = 88;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(
        pdta,
        requestFor(
            native.data(), native.size(),
            modulation.data(), modulation.size(),
            playback.data(), playback.size(),
            dependencies.data(), dependencies.size(),
            &snapshot));
    assert(result.status == PlaybackPrepareStatus::ZoneCapacityExceeded);
    assert(result.required_native_capacity == 200);
    assert(result.native_zone_count == 200);
    assert(result.required_zone_capacity == 200);
    assert(result.zone_count == 0);
    assert(snapshot.token == 88);
}

void testDependencyCapacityIsExplicitAndModulatorPassMatchesNativeZones() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 256> native{};
    std::array<ResolvedZoneModulation, 256> modulation{};
    std::array<PlaybackZone, 256> playback{};
    std::array<SampleId, 1> dependencies{};
    PlaybackProgramSnapshot snapshot{};
    snapshot.token = 55;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(
        pdta,
        requestFor(
            native.data(), native.size(),
            modulation.data(), modulation.size(),
            playback.data(), playback.size(),
            dependencies.data(), dependencies.size(),
            &snapshot));
    assert(result.status == PlaybackPrepareStatus::DependencyCapacityExceeded);
    assert(result.resolve_status == ResolveProgramStatus::Ok);
    assert(result.modulator_resolve_status == ZoneModulatorResolveStatus::Ok);
    assert(result.native_zone_count == 200);
    assert(result.zone_count == 200);
    assert(result.stereo_pair_count == 0);
    assert(result.supported_modulator_records > 0);

    std::uint32_t expected_supported = 0;
    std::uint32_t expected_unsupported = 0;
    for (std::size_t index = 0; index < result.native_zone_count; ++index) {
        expected_supported += modulation[index].supported_velocity_attenuation_records;
        expected_supported += modulation[index].supported_velocity_filter_records;
        expected_supported += modulation[index].supported_velocity_dsp_records;
        expected_supported += modulation[index].supported_note_on_source_records;
        expected_supported += modulation[index].supported_channel_control_records;
        expected_supported += modulation[index].supported_vibrato_records;
        expected_supported += modulation[index].supported_effects_send_records;
        expected_unsupported += modulation[index].unsupported_modulator_records;
    }
    assert(result.supported_modulator_records == expected_supported);
    assert(result.unsupported_modulator_records == expected_unsupported);
    assert(result.unsupported_modulator_records == 0);

    assert(result.required_dependency_capacity > dependencies.size());
    assert(snapshot.token == 55);
}

void testModulationCapacityParticipatesInZonePreflight() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> native{};
    std::array<PlaybackZone, 8> playback{};
    std::array<SampleId, 8> dependencies{};
    PlaybackProgramSnapshot snapshot{};
    snapshot.token = 66;

    PlaybackPrepareRequest request = requestFor(
        native.data(), native.size(),
        nullptr, 0,
        playback.data(), playback.size(),
        dependencies.data(), dependencies.size(),
        &snapshot);

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assert(result.status == PlaybackPrepareStatus::ZoneCapacityExceeded);
    assert(result.required_native_capacity == 1);
    assert(result.required_zone_capacity == 1);
    assert(snapshot.token == 66);
}

void testMissingPresetSurfacesResolverStatus() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> native{};
    std::array<ResolvedZoneModulation, 8> modulation{};
    std::array<PlaybackZone, 8> playback{};
    std::array<SampleId, 8> dependencies{};
    PlaybackProgramSnapshot snapshot{};
    PlaybackPrepareRequest request = requestFor(
        native.data(), native.size(),
        modulation.data(), modulation.size(),
        playback.data(), playback.size(),
        dependencies.data(), dependencies.size(),
        &snapshot);
    request.program = 127;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assert(result.status == PlaybackPrepareStatus::ResolveFailed);
    assert(result.resolve_status == ResolveProgramStatus::PresetNotFound);
    assert(result.zone_count == 0);
}

void testInvalidIdentityDoesNotTouchOutputs() {
    NativeResolvedZone native{};
    ResolvedZoneModulation modulation{};
    PlaybackZone playback{};
    SampleId dependency = 0;
    PlaybackProgramSnapshot snapshot{};
    snapshot.token = 91;

    Sf2PdtaReader pdta;
    PlaybackPrepareRequest request = requestFor(
        &native, 1,
        &modulation, 1,
        &playback, 1,
        &dependency, 1,
        &snapshot);
    request.token = 0;

    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assert(result.status == PlaybackPrepareStatus::InvalidArgument);
    assert(snapshot.token == 91);
}

}  // namespace

int main() {
    testFmProgramPreparesQualifiedSnapshotAndResolvedDefaultRoutes();
    testStrictQualificationAcceptsEffectsCompleteProgram();
    testCheckedInFmBaselineAllPresetsStrictlyPrepare();
    testFullFmGm128MelodicProgramsStrictlyPrepare();
    testFullFmStandardDrumKitStrictlyPreparesAndCoversGmKeys();
    testGeneralUserReportsRequiredZoneCapacityBeforeResolvingScratch();
    testDependencyCapacityIsExplicitAndModulatorPassMatchesNativeZones();
    testModulationCapacityParticipatesInZonePreflight();
    testMissingPresetSurfacesResolverStatus();
    testInvalidIdentityDoesNotTouchOutputs();
    std::cout << "SF2 playback program preparer host checks passed\n";
    return 0;
}