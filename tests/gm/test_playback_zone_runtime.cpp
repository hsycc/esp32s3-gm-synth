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

namespace {

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

PlaybackProgramSnapshot playbackProgram(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token) {
    PlaybackProgramSnapshot snapshot{};
    snapshot.channel = channel;
    snapshot.bank = 0;
    snapshot.program = program;
    snapshot.token = token;
    snapshot.bank_generation = 1;
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
    PlaybackProgramSnapshot program = playbackProgram(2, 10, 1);
    program.zone_count = 3;
    program.zones[0] = loopingZone(0);
    program.zones[0].exclusive_class = 7;
    program.zones[1] = loopingZone(1);
    program.zones[1].exclusive_class = 7;
    program.zones[2] = loopingZone(2);
    program.zones[2].velocity_low = 81;
    program.zones[2].velocity_high = 127;
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
    PlaybackProgramSnapshot program = playbackProgram(9, 0, 1);
    program.zone_count = 1;
    program.zones[0] = loopingZone(0);
    program.zones[0].key_low = 35;
    program.zones[0].key_high = 81;
    program.zones[0].root_phase_increment_q32 = kPhaseOneQ32 / 2u;
    program.zones[0].key_tracking = false;

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 9, 60, 70, 1, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].phase_increment_q32 == kPhaseOneQ32 / 2u);
}

void testResolverRejectsMoreThanHardVoiceCapWithoutPartialBatch() {
    PlaybackProgramSnapshot program = playbackProgram(1, 0, 1);
    program.zone_count = static_cast<std::uint16_t>(VoicePool::kHardVoiceCap + 1u);
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        program.zones[i] = loopingZone(static_cast<std::uint32_t>(i % 4u));
    }
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 1, 60, 64, 5, &batch) ==
           ZoneResolveStatus::TooManyMatches);
    assert(batch.count == 0);
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

    PlaybackProgramSnapshot program = playbackProgram(2, 0, 1);
    program.zone_count = 2;
    program.zones[0] = loopingZone(0);
    program.zones[0].exclusive_class = 7;
    program.zones[1] = loopingZone(1);
    program.zones[1].exclusive_class = 7;

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

    PlaybackProgramSnapshot program = playbackProgram(1, 0, 1);
    program.zone_count = 1;
    program.zones[0] = loopingZone(0);
    program.zones[0].key_low = 60;
    program.zones[0].key_high = 60;
    program.zones[0].envelope.release_frames = 2;

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

}  // namespace

int main() {
    testResolverSelectsKeyVelocityLayersAndPitch();
    testFixedPitchZoneDoesNotTrackKey();
    testResolverRejectsMoreThanHardVoiceCapWithoutPartialBatch();
    testRealtimeApplierSharesNoteInstanceAcrossLayersAndChokesOldTrigger();
    testProgramRuntimeCommitRequiresMatchingPreparedIdentity();
    testScheduledEventToZoneToVoiceToRenderPath();
    std::cout << "playback zone runtime tests passed\n";
    return 0;
}
