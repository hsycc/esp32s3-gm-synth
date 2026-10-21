#include "gm/PlaybackProgramKeyIndex.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "realtime/BlockEventScheduler.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramKeySpan;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ProgramRuntimeCommitCoordinator;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::realtime::BlockEventScheduler;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::realtime::RealtimeEventQueue;
using gm_synth::realtime::ScheduledAudioBlockRenderer;
using gm_synth::sample::ProgramSnapshot;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::VoiceStealConfig;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct PublishedFixture {
    PublishedSampleDirectory directory;
    std::array<ResidentRecord, 4> records{};

    bool publish(
        std::uint32_t sample_id,
        const std::int16_t* pcm,
        std::uint32_t frames,
        std::uint32_t loop_start,
        std::uint32_t loop_end) {
        assert(sample_id < records.size());
        ResidentRecord& record = records[sample_id];
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = sample_id;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = frames * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = loop_start;
        record.descriptor.loop_end = loop_end;
        return directory.publish(sample_id, &record);
    }
};

PlaybackZone loopingZone(std::uint32_t sample_id) {
    PlaybackZone zone{};
    zone.sample_id = sample_id;
    zone.key_low = 60;
    zone.key_high = 64;
    zone.velocity_low = 1;
    zone.velocity_high = 80;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.key_tracking = true;
    zone.loop_mode = LoopMode::Continuous;
    zone.gain_left_q15 = kQ15Unity;
    zone.gain_right_q15 = kQ15Unity;
    zone.envelope.attack_frames = 0;
    zone.envelope.decay_frames = 0;
    zone.envelope.sustain_q15 = kQ15Unity;
    zone.envelope.release_frames = 4;
    return zone;
}

VoiceStartParams directVoiceParams() {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 2;
    params.key = 60;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.gain_left_q15 = kQ15Unity;
    params.gain_right_q15 = kQ15Unity;
    params.envelope.attack_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = kQ15Unity;
    params.envelope.release_frames = 8;
    return params;
}

PlaybackProgramSnapshot playbackProgram(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token,
    const PlaybackZone* zones = nullptr,
    std::uint16_t zone_count = 0) {
    PlaybackProgramSnapshot snapshot{};
    snapshot.channel = channel;
    snapshot.bank = 0;
    snapshot.program = program;
    snapshot.token = token;
    snapshot.bank_generation = 1;
    snapshot.zones = zones;
    snapshot.zone_count = zone_count;
    return snapshot;
}

ProgramSnapshot residencyProgram(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token) {
    ProgramSnapshot snapshot{};
    snapshot.channel = channel;
    snapshot.bank = 0;
    snapshot.program = program;
    snapshot.token = token;
    snapshot.bank_generation = 1;
    snapshot.dependency_count = 0;
    return snapshot;
}

void testResolverSelectsKeyVelocityLayersAndPitch() {
    std::array<PlaybackZone, 3> zones{};
    zones[0] = loopingZone(0);
    zones[0].exclusive_class = 7;
    zones[1] = loopingZone(1);
    zones[1].exclusive_class = 7;
    zones[2] = loopingZone(2);
    zones[2].velocity_low = 81;
    zones[2].velocity_high = 127;
    PlaybackProgramSnapshot program = playbackProgram(2, 10, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 62, 70, 42, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 2);
    const std::uint64_t expected = ZoneResolver::transposeSemitones(kPhaseOneQ32, 2);
    for (std::size_t i = 0; i < batch.count; ++i) {
        assert(batch.voices[i].exclusive_class == 7);
        assert(batch.voices[i].note_instance_id == 42);
        assert(batch.voices[i].phase_increment_q32 == expected);
    }

    assert(resolver.resolveNoteOn(program, 2, 62, 100, 43, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].sample_id == 2);
    assert(batch.voices[0].exclusive_class == 0);
    assert(resolver.resolveNoteOn(program, 2, 70, 70, 44, &batch) == ZoneResolveStatus::NoMatch);
    assert(batch.count == 0);
}

void testFixedPitchZoneDoesNotTrackKey() {
    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_low = 35;
    zones[0].key_high = 81;
    zones[0].root_phase_increment_q32 = kPhaseOneQ32 / 2u;
    zones[0].key_tracking = false;
    PlaybackProgramSnapshot program = playbackProgram(9, 0, 1, zones.data(), zones.size());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 9, 60, 70, 1, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].phase_increment_q32 == kPhaseOneQ32 / 2u);
}

void testResolverPropagatesPerZoneSampleRegion() {
    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 2;
    zones[0].sample_region.end_frame = 9;
    zones[0].sample_region.loop_start_frame = 3;
    zones[0].sample_region.loop_end_frame = 8;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 64, 11, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    const auto& region = batch.voices[0].sample_region;
    assert(region.enabled);
    assert(region.start_frame == 2);
    assert(region.end_frame == 9);
    assert(region.loop_start_frame == 3);
    assert(region.loop_end_frame == 8);
}

void testResolverRejectsMoreThanHardVoiceCapWithoutPartialBatch() {
    std::array<PlaybackZone, VoicePool::kHardVoiceCap + 1u> zones{};
    for (std::size_t i = 0; i < zones.size(); ++i) {
        zones[i] = loopingZone(static_cast<std::uint32_t>(i % 4u));
    }
    PlaybackProgramSnapshot program = playbackProgram(
        1,
        0,
        1,
        zones.data(),
        static_cast<std::uint16_t>(zones.size()));
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 1, 60, 64, 5, &batch) ==
           ZoneResolveStatus::TooManyMatches);
    assert(batch.count == 0);
}

void testSnapshotRejectsMissingZoneStorage() {
    PlaybackProgramSnapshot program = playbackProgram(0, 0, 1, nullptr, 1);
    assert(!program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 0, 60, 64, 1, &batch) ==
           ZoneResolveStatus::InvalidArgument);
}

void testSnapshotRejectsInvalidEnabledSampleRegion() {
    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 4;
    zones[0].sample_region.end_frame = 4;
    zones[0].sample_region.loop_start_frame = 4;
    zones[0].sample_region.loop_end_frame = 4;
    assert(!playbackProgram(0, 0, 1, zones.data(), zones.size()).valid());

    zones[0].sample_region.start_frame = 2;
    zones[0].sample_region.end_frame = 8;
    zones[0].sample_region.loop_start_frame = 1;
    zones[0].sample_region.loop_end_frame = 6;
    assert(!playbackProgram(0, 0, 1, zones.data(), zones.size()).valid());

    zones[0].sample_region.loop_start_frame = 3;
    zones[0].sample_region.loop_end_frame = 9;
    assert(!playbackProgram(0, 0, 1, zones.data(), zones.size()).valid());

    zones[0].sample_region.loop_start_frame = 3;
    zones[0].sample_region.loop_end_frame = 7;
    assert(playbackProgram(0, 0, 1, zones.data(), zones.size()).valid());

    zones[0].loop_mode = LoopMode::None;
    zones[0].sample_region.loop_start_frame = 100;
    zones[0].sample_region.loop_end_frame = 1;
    assert(playbackProgram(0, 0, 1, zones.data(), zones.size()).valid());
}

void testEnabledSampleRegionBoundsVoicePlayback() {
    constexpr std::array<std::int16_t, 6> pcm{10, 100, 200, 300, 400, 500};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));

    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_high = 60;
    zones[0].loop_mode = LoopMode::None;
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 1;
    zones[0].sample_region.end_frame = 4;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 64, 1, &batch) == ZoneResolveStatus::Ok);
    VoicePool voices(fixture.directory);
    assert(voices.start(batch.voices[0]));

    std::array<std::int32_t, 10> out{};
    voices.renderAdd(out.data(), 5);
    const std::array<std::int32_t, 5> expected{100, 200, 300, 0, 0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(out[i * 2u] == expected[i]);
        assert(out[i * 2u + 1u] == expected[i]);
    }
    assert(voices.activeVoiceCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testSampleRegionInterpolationNeverReadsPastEnd() {
    constexpr std::array<std::int16_t, 4> pcm{0, 1000, 2000, 30000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));

    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_high = 60;
    zones[0].loop_mode = LoopMode::None;
    zones[0].root_phase_increment_q32 = kPhaseOneQ32 / 2u;
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 1;
    zones[0].sample_region.end_frame = 3;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 64, 1, &batch) == ZoneResolveStatus::Ok);
    VoicePool voices(fixture.directory);
    assert(voices.start(batch.voices[0]));

    std::array<std::int32_t, 8> out{};
    voices.renderAdd(out.data(), 4);
    const std::array<std::int32_t, 4> expected{1000, 1500, 2000, 2000};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(out[i * 2u] == expected[i]);
        assert(out[i * 2u + 1u] == expected[i]);
    }
    assert(voices.activeVoiceCount() == 0);
}

void testEnabledSampleRegionUsesPerZoneLoop() {
    constexpr std::array<std::int16_t, 6> pcm{0, 100, 200, 300, 400, 500};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));

    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_high = 60;
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 1;
    zones[0].sample_region.end_frame = 5;
    zones[0].sample_region.loop_start_frame = 2;
    zones[0].sample_region.loop_end_frame = 4;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 64, 1, &batch) == ZoneResolveStatus::Ok);
    VoicePool voices(fixture.directory);
    assert(voices.start(batch.voices[0]));

    std::array<std::int32_t, 12> out{};
    voices.renderAdd(out.data(), 6);
    const std::array<std::int32_t, 6> expected{100, 200, 300, 200, 300, 200};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(out[i * 2u] == expected[i]);
        assert(out[i * 2u + 1u] == expected[i]);
    }
    assert(voices.activeVoiceCount() == 1);
    voices.allSoundOff();
}

void testUntilReleaseLeavesRegionLoopAndStopsAtRegionEnd() {
    constexpr std::array<std::int16_t, 7> pcm{0, 100, 200, 300, 400, 500, 30000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));

    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_high = 60;
    zones[0].loop_mode = LoopMode::UntilRelease;
    zones[0].envelope.release_frames = 16;
    zones[0].sample_region.enabled = true;
    zones[0].sample_region.start_frame = 1;
    zones[0].sample_region.end_frame = 6;
    zones[0].sample_region.loop_start_frame = 2;
    zones[0].sample_region.loop_end_frame = 4;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 64, 1, &batch) == ZoneResolveStatus::Ok);
    VoicePool voices(fixture.directory);
    assert(voices.start(batch.voices[0]));

    std::array<std::int32_t, 8> before{};
    voices.renderAdd(before.data(), 4);
    const std::array<std::int32_t, 4> expected_before{100, 200, 300, 200};
    for (std::size_t i = 0; i < expected_before.size(); ++i) {
        assert(before[i * 2u] == expected_before[i]);
        assert(before[i * 2u + 1u] == expected_before[i]);
    }

    assert(voices.noteOff(2, 60) == 1);
    std::array<std::int32_t, 8> after{};
    voices.renderAdd(after.data(), 4);
    assert(after[0] == 300 && after[1] == 300);
    assert(after[2] > 0 && after[3] > 0);
    assert(after[4] > 0 && after[5] > 0);
    assert(after[6] == 0 && after[7] == 0);
    assert(voices.activeVoiceCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testVoiceRejectsInvalidSampleRegionsWithoutLeakingRefs() {
    constexpr std::array<std::int16_t, 4> pcm{10, 20, 30, 40};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));
    VoicePool voices(fixture.directory);

    auto params = directVoiceParams();
    params.sample_region.enabled = true;
    params.sample_region.start_frame = 2;
    params.sample_region.end_frame = 2;
    assert(!voices.start(params));
    assert(fixture.directory.stats().live_voice_refs == 0);

    params = directVoiceParams();
    params.sample_region.enabled = true;
    params.sample_region.start_frame = 1;
    params.sample_region.end_frame = 5;
    assert(!voices.start(params));
    assert(fixture.directory.stats().live_voice_refs == 0);

    params = directVoiceParams();
    params.loop_mode = LoopMode::Continuous;
    params.sample_region.enabled = true;
    params.sample_region.start_frame = 1;
    params.sample_region.end_frame = 4;
    params.sample_region.loop_start_frame = 0;
    params.sample_region.loop_end_frame = 3;
    assert(!voices.start(params));
    assert(fixture.directory.stats().live_voice_refs == 0);

    params.sample_region.loop_start_frame = 2;
    params.sample_region.loop_end_frame = 5;
    assert(!voices.start(params));
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testDisabledSampleRegionPreservesDescriptorPlayback() {
    constexpr std::array<std::int16_t, 4> pcm{10, 20, 30, 40};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 0));
    VoicePool voices(fixture.directory);

    auto params = directVoiceParams();
    params.sample_region.enabled = false;
    params.sample_region.start_frame = 99;
    params.sample_region.end_frame = 1;
    params.sample_region.loop_start_frame = 88;
    params.sample_region.loop_end_frame = 2;
    assert(voices.start(params));

    std::array<std::int32_t, 10> out{};
    voices.renderAdd(out.data(), 5);
    const std::array<std::int32_t, 5> expected{10, 20, 30, 40, 0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(out[i * 2u] == expected[i]);
        assert(out[i * 2u + 1u] == expected[i]);
    }
    assert(voices.activeVoiceCount() == 0);
}

void testRealtimeApplierSharesNoteInstanceAcrossLayersAndChokesOldTrigger() {
    constexpr std::array<std::int16_t, 16> pcm_a{
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000,
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    constexpr std::array<std::int16_t, 16> pcm_b{
        500, 500, 500, 500, 500, 500, 500, 500,
        500, 500, 500, 500, 500, 500, 500, 500};

    PublishedFixture fixture;
    assert(fixture.publish(0, pcm_a.data(), pcm_a.size(), 0, pcm_a.size()));
    assert(fixture.publish(1, pcm_b.data(), pcm_b.size(), 0, pcm_b.size()));

    std::array<PlaybackZone, 2> zones{loopingZone(0), loopingZone(1)};
    zones[0].exclusive_class = 7;
    zones[1].exclusive_class = 7;
    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1, zones.data(), zones.size());

    PlaybackProgramSnapshotExchange exchange;
    assert(exchange.setInitialActive(&program));
    RealtimeProgramEventApplier applier(exchange);
    VoicePool voices(fixture.directory);
    VoiceStealConfig config{};
    config.enabled = false;
    config.fade_frames = 4;
    config.max_tail_voices = 4;
    voices.setVoiceStealConfig(config);

    RealtimeEvent note_on{};
    note_on.type = EventType::NoteOn;
    note_on.channel = 2;
    note_on.data1 = 60;
    note_on.data2 = 70;
    assert(applier.apply(note_on, voices));
    assert(voices.activeVoiceCount() == 2);
    assert(voices.activeExclusiveTailCount() == 0);

    assert(applier.apply(note_on, voices));
    assert(voices.activeVoiceCount() == 4);
    assert(voices.activeExclusiveTailCount() == 2);

    std::array<std::int32_t, 8> fade{};
    voices.renderAdd(fade.data(), 4);
    assert(voices.activeVoiceCount() == 2);
    assert(voices.activeExclusiveTailCount() == 0);

    RealtimeEvent note_off{};
    note_off.type = EventType::NoteOff;
    note_off.channel = 2;
    note_off.data1 = 60;
    assert(applier.apply(note_off, voices));
    std::array<std::int32_t, 12> release{};
    voices.renderAdd(release.data(), 6);
    assert(voices.activeVoiceCount() == 0);

    const auto stats = applier.stats();
    assert(stats.note_on_events == 2);
    assert(stats.note_off_events == 1);
    assert(stats.voices_resolved == 4);
    assert(stats.voices_started == 4);
    assert(stats.voice_start_failures == 0);
}

void testProgramRuntimeCommitRequiresMatchingPreparedIdentity() {
    ProgramSnapshotExchange residency;
    PlaybackProgramSnapshotExchange playback;
    ProgramRuntimeCommitCoordinator coordinator(residency, playback);

    ProgramSnapshot resident_a = residencyProgram(3, 4, 1);
    PlaybackProgramSnapshot play_a = playbackProgram(3, 4, 1);
    assert(residency.setInitialActive(&resident_a));
    assert(playback.setInitialActive(&play_a));
    assert(coordinator.activeCompatible(3));

    ProgramSnapshot resident_b = residencyProgram(3, 5, 2);
    PlaybackProgramSnapshot play_b = playbackProgram(3, 5, 2);
    assert(residency.publishReady(&resident_b));
    assert(playback.publishReady(&play_b));
    assert(coordinator.readyCompatible(3));
    assert(coordinator.commitAtBlockBoundary(3));
    assert(coordinator.activeCompatible(3));
    assert(residency.active(3) == &resident_b);
    assert(playback.active(3) == &play_b);
    assert(residency.acknowledgeCommit(3, 2).valid());
    assert(playback.acknowledgeCommit(3, 2).valid());

    ProgramSnapshotExchange mismatch_residency;
    PlaybackProgramSnapshotExchange mismatch_playback;
    ProgramRuntimeCommitCoordinator mismatch(mismatch_residency, mismatch_playback);
    ProgramSnapshot mismatch_a = residencyProgram(4, 1, 10);
    PlaybackProgramSnapshot mismatch_play_a = playbackProgram(4, 1, 10);
    assert(mismatch_residency.setInitialActive(&mismatch_a));
    assert(mismatch_playback.setInitialActive(&mismatch_play_a));

    ProgramSnapshot mismatch_b = residencyProgram(4, 2, 11);
    PlaybackProgramSnapshot mismatch_play_b = playbackProgram(4, 2, 12);
    assert(mismatch_residency.publishReady(&mismatch_b));
    assert(mismatch_playback.publishReady(&mismatch_play_b));
    assert(!mismatch.readyCompatible(4));
    assert(!mismatch.commitAtBlockBoundary(4));
    assert(mismatch_residency.active(4) == &mismatch_a);
    assert(mismatch_playback.active(4) == &mismatch_play_a);
}

void testScheduledEventToZoneToVoiceToRenderPath() {
    constexpr std::array<std::int16_t, 8> pcm{
        1200, 1200, 1200, 1200, 1200, 1200, 1200, 1200};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    std::array<PlaybackZone, 1> zones{loopingZone(0)};
    zones[0].key_low = 60;
    zones[0].key_high = 60;
    zones[0].envelope.release_frames = 2;
    PlaybackProgramSnapshot program = playbackProgram(1, 0, 1, zones.data(), zones.size());

    PlaybackProgramSnapshotExchange programs;
    assert(programs.setInitialActive(&program));
    RealtimeProgramEventApplier applier(programs);
    VoicePool voices(fixture.directory);
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler(queue);
    ScheduledAudioBlockRenderer renderer(
        voices,
        scheduler,
        &RealtimeProgramEventApplier::callback,
        &applier);

    RealtimeEvent on{};
    on.timestamp_frame = 2;
    on.type = EventType::NoteOn;
    on.channel = 1;
    on.data1 = 60;
    on.data2 = 70;
    assert(queue.tryPush(on));

    RealtimeEvent off{};
    off.timestamp_frame = 6;
    off.type = EventType::NoteOff;
    off.channel = 1;
    off.data1 = 60;
    assert(queue.tryPush(off));

    std::array<std::int16_t, ScheduledAudioBlockRenderer::kSamplesPerBlock> output{};
    renderer.render(0, output.data());

    assert(output[0] == 0);
    assert(output[2] == 0);
    assert(output[4] > 0);
    assert(output[10] > 0);
    assert(output[20] == 0);
    assert(renderer.stats().events_attempted == 2);
    assert(renderer.stats().events_applied == 2);
    assert(renderer.stats().events_rejected == 0);
    assert(voices.activeVoiceCount() == 0);
}


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

}  // namespace

int main() {
    testResolverSelectsKeyVelocityLayersAndPitch();
    testFixedPitchZoneDoesNotTrackKey();
    testResolverPropagatesPerZoneSampleRegion();
    testResolverRejectsMoreThanHardVoiceCapWithoutPartialBatch();
    testSnapshotRejectsMissingZoneStorage();
    testSnapshotRejectsInvalidEnabledSampleRegion();
    testEnabledSampleRegionBoundsVoicePlayback();
    testSampleRegionInterpolationNeverReadsPastEnd();
    testEnabledSampleRegionUsesPerZoneLoop();
    testUntilReleaseLeavesRegionLoopAndStopsAtRegionEnd();
    testVoiceRejectsInvalidSampleRegionsWithoutLeakingRefs();
    testDisabledSampleRegionPreservesDescriptorPlayback();
    testRealtimeApplierSharesNoteInstanceAcrossLayersAndChokesOldTrigger();
    testProgramRuntimeCommitRequiresMatchingPreparedIdentity();
    testResolverUsesKeyIndexForLargeProgram();
    testScheduledEventToZoneToVoiceToRenderPath();
    std::cout << "playback zone runtime tests passed\n";
    return 0;
}
