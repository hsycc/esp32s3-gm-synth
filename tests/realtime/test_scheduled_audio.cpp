#include "../../firmware/realtime/ScheduledAudioBlockRenderer.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::realtime::BlockEventScheduler;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::realtime::RealtimeEventQueue;
using gm_synth::realtime::ScheduledAudioBlockRenderer;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 128> pcm{};

    PublishedFixture() {
        pcm.fill(1000);
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm.data();
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = 0;
        record.descriptor.loop_end = pcm.size();
        assert(directory.publish(0, &record));
    }
};

struct DispatchContext {
    VoiceStartParams params{};

    DispatchContext() {
        params.sample_id = 0;
        params.channel = 0;
        params.key = 60;
        params.velocity = 100;
        params.phase_increment_q32 = kPhaseOneQ32;
        params.loop_mode = LoopMode::Continuous;
        params.gain_left_q15 = kQ15Unity;
        params.gain_right_q15 = kQ15Unity;
        params.envelope.attack_frames = 0;
        params.envelope.decay_frames = 0;
        params.envelope.sustain_q15 = kQ15Unity;
        params.envelope.release_frames = 0;
    }
};

bool applyEvent(void* opaque, const RealtimeEvent& event, VoicePool& voices) {
    auto& context = *static_cast<DispatchContext*>(opaque);
    switch (event.type) {
        case EventType::NoteOn: {
            auto params = context.params;
            params.channel = event.channel;
            params.key = event.data1;
            params.velocity = event.data2;
            return voices.start(params);
        }
        case EventType::NoteOff:
            return voices.noteOff(event.channel, event.data1) != 0;
        case EventType::ControlChange:
        case EventType::ProgramChange:
        default:
            return false;
    }
}

RealtimeEvent note(
    std::uint64_t frame,
    std::uint8_t key,
    EventType type = EventType::NoteOn) {
    RealtimeEvent event{};
    event.timestamp_frame = frame;
    event.type = type;
    event.channel = 0;
    event.data1 = key;
    event.data2 = 100;
    return event;
}

void assertStereoFrame(
    const std::array<std::int16_t, 128>& output,
    std::size_t frame,
    std::int16_t expected) {
    assert(output[frame * 2] == expected);
    assert(output[frame * 2 + 1] == expected);
}

void testNoteOnAndOffAreSampleAccurate() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler(queue);
    DispatchContext dispatch;
    ScheduledAudioBlockRenderer renderer(voices, scheduler, applyEvent, &dispatch);

    assert(queue.tryPush(note(1010, 60, EventType::NoteOn)));
    assert(queue.tryPush(note(1020, 60, EventType::NoteOff)));

    std::array<std::int16_t, 128> output{};
    renderer.render(1000, output.data());

    for (std::size_t frame = 0; frame < 10; ++frame) {
        assertStereoFrame(output, frame, 0);
    }
    for (std::size_t frame = 10; frame < 20; ++frame) {
        assertStereoFrame(output, frame, 1000);
    }
    for (std::size_t frame = 20; frame < 64; ++frame) {
        assertStereoFrame(output, frame, 0);
    }

    assert(voices.activeVoiceCount() == 0);
    assert(renderer.stats().events_applied == 2);
    assert(renderer.stats().render_segments == 3);
}

void testBlockEndEventMovesToNextBlockOffsetZero() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler(queue);
    DispatchContext dispatch;
    ScheduledAudioBlockRenderer renderer(voices, scheduler, applyEvent, &dispatch);

    assert(queue.tryPush(note(1064, 61)));
    std::array<std::int16_t, 128> first{};
    renderer.render(1000, first.data());
    for (std::size_t frame = 0; frame < 64; ++frame) {
        assertStereoFrame(first, frame, 0);
    }
    assert(queue.size() == 1);

    std::array<std::int16_t, 128> second{};
    renderer.render(1064, second.data());
    assertStereoFrame(second, 0, 1000);
    assert(queue.empty());
    voices.allSoundOff();
}

void testEqualTimestampPreservesFifo() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler(queue);
    DispatchContext dispatch;
    ScheduledAudioBlockRenderer renderer(voices, scheduler, applyEvent, &dispatch);

    assert(queue.tryPush(note(1005, 62, EventType::NoteOn)));
    assert(queue.tryPush(note(1005, 62, EventType::NoteOff)));

    std::array<std::int16_t, 128> output{};
    renderer.render(1000, output.data());
    for (std::size_t frame = 0; frame < 64; ++frame) {
        assertStereoFrame(output, frame, 0);
    }
    assert(renderer.stats().events_applied == 2);
}

void testLateEventClampsToFrameZero() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler(queue);
    DispatchContext dispatch;
    ScheduledAudioBlockRenderer renderer(voices, scheduler, applyEvent, &dispatch);

    assert(queue.tryPush(note(900, 63)));
    std::array<std::int16_t, 128> output{};
    renderer.render(1000, output.data());
    assertStereoFrame(output, 0, 1000);
    assert(scheduler.stats().late_events == 1);
    voices.allSoundOff();
}

void testQueueOrderingAndCapacityAreExplicit() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(100, 60)));
    assert(!queue.tryPush(note(99, 61)));
    assert(queue.stats().rejected_out_of_order == 1);

    RealtimeEvent popped{};
    assert(queue.tryPop(&popped));
    for (std::size_t i = 0; i < RealtimeEventQueue::kCapacity; ++i) {
        assert(queue.tryPush(note(100 + i, static_cast<std::uint8_t>(i & 0x7f))));
    }
    assert(!queue.tryPush(note(100 + RealtimeEventQueue::kCapacity, 1)));
    assert(queue.stats().rejected_full == 1);
}

}  // namespace

int main() {
    testNoteOnAndOffAreSampleAccurate();
    testBlockEndEventMovesToNextBlockOffsetZero();
    testEqualTimestampPreservesFifo();
    testLateEventClampsToFrameZero();
    testQueueOrderingAndCapacityAreExplicit();
    std::cout << "scheduled audio tests passed\n";
    return 0;
}
