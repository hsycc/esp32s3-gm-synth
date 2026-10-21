from pathlib import Path


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise SystemExit(f"{path}: expected one {label}, found {text.count(old)}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# 1) Prove the actual realtime resolver handles a 720-zone snapshot through the
# immutable key index and rejects the same large snapshot without one.
path = Path("tests/gm/test_playback_zone_runtime.cpp")
replace_once(
    path,
    '#include "gm/PlaybackProgramSnapshot.h"\n#include "gm/ZoneResolver.h"',
    '#include "gm/PlaybackProgramKeyIndex.h"\n#include "gm/PlaybackProgramSnapshot.h"\n#include "gm/ZoneResolver.h"',
    "runtime include block",
)
replace_once(path, '#include <iostream>\n', '#include <iostream>\n#include <vector>\n', "runtime vector include")
replace_once(
    path,
    'using gm_synth::gm::PlaybackProgramSnapshot;\n',
    'using gm_synth::gm::PlaybackProgramKeySpan;\nusing gm_synth::gm::PlaybackProgramSnapshot;\n',
    "runtime key-span using",
)
insert = r'''
void testResolverUsesKeyIndexForLargeProgram() {
    constexpr std::size_t kZoneCount = 720;
    constexpr std::uint8_t kKey = 42;

    std::vector<PlaybackZone> zones(kZoneCount);
    for (std::size_t index = 0; index < zones.size(); ++index) {
        PlaybackZone& zone = zones[index];
        zone.sample_id = static_cast<std::uint32_t>(index);
        const std::uint8_t key = static_cast<std::uint8_t>(index % 128u);
        zone.key_low = key;
        zone.key_high = key;
        zone.velocity_low = 1;
        zone.velocity_high = 127;
        zone.root_key = key;
        zone.key_tracking = false;
        zone.root_phase_increment_q32 = kPhaseOneQ32;
        assert(zone.valid());
    }

    std::array<PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> spans{};
    std::vector<std::uint16_t> references(kZoneCount);
    const auto build = gm_synth::gm::buildPlaybackProgramKeyIndex(
        zones.data(),
        zones.size(),
        spans.data(),
        references.data(),
        references.size());
    assert(build.status == gm_synth::gm::PlaybackProgramKeyIndexBuildStatus::Ok);
    assert(build.required_references == kZoneCount);
    assert(build.max_candidates_per_key == 6);

    PlaybackProgramSnapshot snapshot{};
    snapshot.channel = 3;
    snapshot.bank = 0;
    snapshot.program = 17;
    snapshot.token = 101;
    snapshot.bank_generation = 11;
    snapshot.zones = zones.data();
    snapshot.zone_count = static_cast<std::uint16_t>(zones.size());
    snapshot.key_spans = spans.data();
    snapshot.key_zone_indices = references.data();
    snapshot.key_reference_count = build.required_references;
    assert(snapshot.valid());

    ResolvedVoiceBatch batch{};
    ZoneResolver resolver;
    assert(resolver.resolveNoteOn(snapshot, 3, kKey, 100, 9001, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 6);
    constexpr std::array<std::uint32_t, 6> kExpected{{42, 170, 298, 426, 554, 682}};
    for (std::size_t index = 0; index < kExpected.size(); ++index) {
        assert(batch.voices[index].sample_id == kExpected[index]);
    }

    PlaybackProgramSnapshot missing_index = snapshot;
    missing_index.key_spans = nullptr;
    missing_index.key_zone_indices = nullptr;
    missing_index.key_reference_count = 0;
    assert(!missing_index.valid());
    batch.clear();
    assert(resolver.resolveNoteOn(missing_index, 3, kKey, 100, 9002, &batch) ==
           ZoneResolveStatus::InvalidArgument);
    assert(batch.count == 0);
}

'''
replace_once(path, '}  // namespace\n\nint main() {', insert + '}  // namespace\n\nint main() {', "runtime test insertion")
replace_once(
    path,
    '    testProgramRuntimeCommitRequiresMatchingPreparedIdentity();\n    testScheduledEventToZoneToVoiceToRenderPath();',
    '    testProgramRuntimeCommitRequiresMatchingPreparedIdentity();\n    testResolverUsesKeyIndexForLargeProgram();\n    testScheduledEventToZoneToVoiceToRenderPath();',
    "runtime main call",
)

# 2) Prove Sf2PlaybackProgramPreparer builds and publishes the persistent index.
path = Path("tests/sf2/test_sf2_playback_program_preparer.cpp")
replace_once(
    path,
    'using gm_synth::gm::PlaybackProgramSnapshot;\n',
    'using gm_synth::gm::PlaybackProgramKeySpan;\nusing gm_synth::gm::PlaybackProgramSnapshot;\n',
    "preparer key-span using",
)
replace_once(
    path,
    '    std::array<SampleId, 8> dependencies{};\n    PlaybackProgramSnapshot snapshot{};\n\n    const auto result = Sf2PlaybackProgramPreparer{}.prepare(\n        pdta,\n        requestFor(\n            native.data(), native.size(),\n            modulation.data(), modulation.size(),\n            playback.data(), playback.size(),\n            dependencies.data(), dependencies.size(),\n            &snapshot));',
    '    std::array<SampleId, 8> dependencies{};\n    std::array<PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};\n    std::array<std::uint16_t, PlaybackProgramSnapshot::kMidiKeyCount> key_references{};\n    PlaybackProgramSnapshot snapshot{};\n\n    PlaybackPrepareRequest request = requestFor(\n        native.data(), native.size(),\n        modulation.data(), modulation.size(),\n        playback.data(), playback.size(),\n        dependencies.data(), dependencies.size(),\n        &snapshot);\n    request.key_spans = key_spans.data();\n    request.key_zone_indices = key_references.data();\n    request.key_zone_index_capacity = key_references.size();\n    const auto result = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);',
    "preparer indexed first request",
)
replace_once(
    path,
    '    assert(result.required_zone_capacity == 1);\n    assert(result.dependency_count == 1);',
    '    assert(result.required_zone_capacity == 1);\n    assert(result.required_key_index_capacity > 0);\n    assert(result.required_key_index_capacity <= key_references.size());\n    assert(result.max_candidates_per_key == 1);\n    assert(result.dependency_count == 1);',
    "preparer index result assertions",
)
replace_once(
    path,
    '    assert(snapshot.zones == playback.data());\n    assert(snapshot.zone_count == 1);\n',
    '    assert(snapshot.zones == playback.data());\n    assert(snapshot.zone_count == 1);\n    assert(snapshot.key_spans == key_spans.data());\n    assert(snapshot.key_zone_indices == key_references.data());\n    assert(snapshot.key_reference_count == result.required_key_index_capacity);\n',
    "preparer snapshot index assertions",
)

# 3) Exercise the Program Change transaction with persistent index storage.
path = Path("tests/gm/test_program_change_control_service.cpp")
replace_once(
    path,
    'using gm_synth::gm::PlaybackProgramSnapshotExchange;\n',
    'using gm_synth::gm::PlaybackProgramSnapshot;\nusing gm_synth::gm::PlaybackProgramSnapshotExchange;\n',
    "program-change snapshot using",
)
replace_once(
    path,
    '    std::vector<gm_synth::gm::PlaybackZone> playback;\n    std::vector<gm_synth::sample::SampleId> dependencies;\n    ProgramChangePrepareWorkspace workspace{};\n\n    WorkspaceStorage()\n        : native(256), modulation(256), playback(256), dependencies(512) {',
    '    std::vector<gm_synth::gm::PlaybackZone> playback;\n    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};\n    std::vector<std::uint16_t> key_zone_indices;\n    std::vector<gm_synth::sample::SampleId> dependencies;\n    ProgramChangePrepareWorkspace workspace{};\n\n    WorkspaceStorage()\n        : native(256), modulation(256), playback(256),\n          key_zone_indices(256u * PlaybackProgramSnapshot::kMidiKeyCount), dependencies(512) {',
    "program-change workspace storage",
)
replace_once(
    path,
    '        workspace.playback_zones = playback.data();\n        workspace.playback_capacity = playback.size();\n        workspace.dependencies = dependencies.data();',
    '        workspace.playback_zones = playback.data();\n        workspace.playback_capacity = playback.size();\n        workspace.key_spans = key_spans.data();\n        workspace.key_zone_indices = key_zone_indices.data();\n        workspace.key_zone_index_capacity = key_zone_indices.size();\n        workspace.dependencies = dependencies.data();',
    "program-change workspace pointers",
)
replace_once(
    path,
    '    request.playback_zones = storage.workspace.playback_zones;\n    request.playback_capacity = storage.workspace.playback_capacity;\n    request.dependencies = storage.workspace.dependencies;',
    '    request.playback_zones = storage.workspace.playback_zones;\n    request.playback_capacity = storage.workspace.playback_capacity;\n    request.key_spans = storage.workspace.key_spans;\n    request.key_zone_indices = storage.workspace.key_zone_indices;\n    request.key_zone_index_capacity = storage.workspace.key_zone_index_capacity;\n    request.dependencies = storage.workspace.dependencies;',
    "program-change initial request index",
)

# 4) Keep M7 live Program Change on the same production storage contract.
path = Path("tests/gm/test_m7_gm128_live_program_change.cpp")
replace_once(
    path,
    'using gm_synth::gm::PlaybackProgramSnapshotExchange;\n',
    'using gm_synth::gm::PlaybackProgramSnapshot;\nusing gm_synth::gm::PlaybackProgramSnapshotExchange;\n',
    "m7-live snapshot using",
)
replace_once(
    path,
    '    std::vector<gm_synth::gm::PlaybackZone> playback;\n    std::vector<SampleId> dependencies;\n    ProgramChangePrepareWorkspace workspace{};\n\n    WorkspaceStorage()\n        : native(256), modulation(256), playback(256), dependencies(512) {',
    '    std::vector<gm_synth::gm::PlaybackZone> playback;\n    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};\n    std::vector<std::uint16_t> key_zone_indices;\n    std::vector<SampleId> dependencies;\n    ProgramChangePrepareWorkspace workspace{};\n\n    WorkspaceStorage()\n        : native(256), modulation(256), playback(256),\n          key_zone_indices(256u * PlaybackProgramSnapshot::kMidiKeyCount), dependencies(512) {',
    "m7-live workspace storage",
)
replace_once(
    path,
    '        workspace.playback_zones = playback.data();\n        workspace.playback_capacity = playback.size();\n        workspace.dependencies = dependencies.data();',
    '        workspace.playback_zones = playback.data();\n        workspace.playback_capacity = playback.size();\n        workspace.key_spans = key_spans.data();\n        workspace.key_zone_indices = key_zone_indices.data();\n        workspace.key_zone_index_capacity = key_zone_indices.size();\n        workspace.dependencies = dependencies.data();',
    "m7-live workspace pointers",
)
replace_once(
    path,
    '    request.playback_zones = storage.workspace.playback_zones;\n    request.playback_capacity = storage.workspace.playback_capacity;\n    request.dependencies = storage.workspace.dependencies;',
    '    request.playback_zones = storage.workspace.playback_zones;\n    request.playback_capacity = storage.workspace.playback_capacity;\n    request.key_spans = storage.workspace.key_spans;\n    request.key_zone_indices = storage.workspace.key_zone_indices;\n    request.key_zone_index_capacity = storage.workspace.key_zone_index_capacity;\n    request.dependencies = storage.workspace.dependencies;',
    "m7-live initial request index",
)

# 5) M7 matrix must opt into the index so it can classify programs above the old
# 256-zone limit instead of rejecting them before semantic qualification.
path = Path("tests/gm/test_sf2_release_matrix.cpp")
replace_once(path, '#include <utility>\n', '#include <utility>\n#include <vector>\n', "matrix vector include")
replace_once(
    path,
    '    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};\n    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};\n',
    '    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};\n    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};\n    std::vector<std::uint16_t> key_zone_indices(\n        PlaybackProgramSnapshot::kMaxZones * PlaybackProgramSnapshot::kMidiKeyCount);\n    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};\n',
    "matrix index storage",
)
replace_once(
    path,
    '        request.playback_zones = playback.data();\n        request.playback_capacity = playback.size();\n        request.dependencies = dependencies.data();',
    '        request.playback_zones = playback.data();\n        request.playback_capacity = playback.size();\n        request.key_spans = key_spans.data();\n        request.key_zone_indices = key_zone_indices.data();\n        request.key_zone_index_capacity = key_zone_indices.size();\n        request.dependencies = dependencies.data();',
    "matrix request index",
)

# Keep the public realtime contract comment aligned with the indexed path.
path = Path("firmware/gm/ZoneResolver.h")
replace_once(
    path,
    '// conversion. The scan is bounded by PlaybackProgramSnapshot::kMaxZones.\n',
    '// conversion. Legacy snapshots scan at most kLinearScanZoneLimit zones;\n// indexed snapshots scan only the immutable candidate span for the incoming key.\n',
    "resolver comment",
)
