#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "sample/PublishedSampleDirectory.h"
#include "sf2/Sf2PlaybackMaterializer.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueDefaultModulatorsPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;

NativeResolvedZone baseNativeZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 0;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.sample_start = 0;
    zone.sample_end = 256;
    zone.loop_start = 0;
    zone.loop_end = 256;
    zone.shdr_start = 0;
    zone.shdr_end = 256;
    zone.shdr_loop_start = 0;
    zone.shdr_loop_end = 256;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    zone.generators.set(54, 1);
    return zone;
}

void testMaterializerCarriesMandatoryVibratoRoutes() {
    NativeResolvedZone native = baseNativeZone();
    ResolvedZoneModulation modulation{};
    PlaybackZone zone{};

    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueDefaultModulatorsPending));
    assert(zone.vibrato_lfo.enabled);
    assert(zone.vibrato_lfo.delay_frames == 43);
    assert(zone.vibrato_lfo.phase_step_q32 != 0);
    assert(zone.vibrato_lfo.static_pitch_depth_cents == 0);
    assert(zone.vibrato_lfo.channel_pressure_to_pitch_cents == 50);
    assert(zone.vibrato_lfo.modulation_wheel_to_pitch_cents == 50);

    modulation.channel_pressure_to_vib_lfo_pitch_cents = 0;
    modulation.modulation_wheel_to_vib_lfo_pitch_cents = 0;
    const auto cancelled = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(cancelled.playablePreview());
    assert(!zone.vibrato_lfo.enabled);
}

PlaybackProgramSnapshot programFor(const PlaybackZone* zone) {
    PlaybackProgramSnapshot program{};
    program.channel = 2;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = 1;
    program.zones = zone;
    program.zone_count = 1;
    return program;
}

PlaybackZone vibratoZone() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.loop_mode = LoopMode::Continuous;
    zone.vibrato_lfo.enabled = true;
    zone.vibrato_lfo.delay_frames = 0;
    zone.vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 26u;  // 1/64 cycle/frame
    zone.vibrato_lfo.static_pitch_depth_cents = 0;
    zone.vibrato_lfo.channel_pressure_to_pitch_cents = 50;
    zone.vibrato_lfo.modulation_wheel_to_pitch_cents = 50;
    return zone;
}

void testResolverSumsCurrentPressureAndModWheelIntoDepth() {
    PlaybackZone zone = vibratoZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    MidiChannelState state{};
    state.resetAll();
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};

    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 1, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].vibrato_lfo.pitch_depth_cents == 0);

    state.modulation_wheel = 127;
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 2, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].vibrato_lfo.pitch_depth_cents == 50);

    state.channel_pressure = 127;
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 3, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].vibrato_lfo.pitch_depth_cents == 100);
}

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 256> pcm{};

    PublishedFixture() {
        for (std::size_t index = 0; index < pcm.size(); ++index) {
            pcm[index] = static_cast<std::int16_t>((index % 64u) * 300 - 9000);
        }
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

void testVoiceTriangleLfoActuallyChangesPhaseIncrement() {
    PublishedFixture fixture;
    VoiceStartParams params{};
    params.sample_id = 0;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.loop_mode = LoopMode::Continuous;
    params.vibrato_lfo.enabled = true;
    params.vibrato_lfo.delay_frames = 0;
    params.vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 26u;
    params.vibrato_lfo.pitch_depth_cents = 100;

    Voice voice;
    assert(voice.start(fixture.directory, params));
    assert(voice.currentPhaseIncrementQ32() == kPhaseOneQ32);

    std::array<std::int32_t, 34> output{};
    const auto result = voice.renderAdd(output.data(), 17);
    assert(result.frames_rendered == 17);
    assert(voice.active());
    assert(voice.currentPhaseIncrementQ32() > kPhaseOneQ32);
}

RealtimeEvent noteOn() {
    RealtimeEvent event{};
    event.type = EventType::NoteOn;
    event.channel = 2;
    event.data1 = 60;
    event.data2 = 100;
    return event;
}

RealtimeEvent cc1(std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = 2;
    event.data1 = 1;
    event.data2 = value;
    return event;
}

RealtimeEvent channelPressure(std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ChannelPressure;
    event.channel = 2;
    event.data1 = value;
    return event;
}

struct DepthSnapshot {
    bool seen = false;
    std::int32_t depth_cents = 0;
};

bool captureDepth(void* opaque, Voice& voice) {
    auto* snapshot = static_cast<DepthSnapshot*>(opaque);
    if (snapshot == nullptr || snapshot->seen) {
        return false;
    }
    snapshot->seen = true;
    snapshot->depth_cents = voice.vibratoLfoConfig().pitch_depth_cents;
    return false;
}

std::int32_t activeDepth(VoicePool& voices) {
    DepthSnapshot snapshot{};
    voices.updateActiveVoices(2, &captureDepth, &snapshot);
    assert(snapshot.seen);
    return snapshot.depth_cents;
}

void testRealtimeControllersRefreshAlreadySoundingVibrato() {
    PublishedFixture fixture;
    PlaybackZone zone = vibratoZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    PlaybackProgramSnapshotExchange programs;
    assert(programs.setInitialActive(&program));
    VoicePool voices{fixture.directory};
    RealtimeProgramEventApplier applier{programs};

    assert(applier.apply(noteOn(), voices));
    assert(activeDepth(voices) == 0);

    assert(applier.apply(cc1(127), voices));
    assert(applier.channelState(2).modulation_wheel == 127);
    assert(activeDepth(voices) == 50);

    assert(applier.apply(channelPressure(127), voices));
    assert(applier.channelState(2).channel_pressure == 127);
    assert(activeDepth(voices) == 100);
    assert(applier.stats().channel_pressure_events == 1);

    RealtimeEvent reset{};
    reset.type = EventType::ControlChange;
    reset.channel = 2;
    reset.data1 = 121;
    reset.data2 = 0;
    assert(applier.apply(reset, voices));
    assert(activeDepth(voices) == 0);
}

}  // namespace

int main() {
    testMaterializerCarriesMandatoryVibratoRoutes();
    testResolverSumsCurrentPressureAndModWheelIntoDepth();
    testVoiceTriangleLfoActuallyChangesPhaseIncrement();
    testRealtimeControllersRefreshAlreadySoundingVibrato();
    std::cout << "SF2 vibrato-LFO runtime checks passed\n";
    return 0;
}
