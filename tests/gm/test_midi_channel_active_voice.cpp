#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "sample/PublishedSampleDirectory.h"
#include "sf2/Sf2ModulationMath.h"
#include "sf2/Sf2PanMath.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ZoneResolver;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::channelEffectsSendSf2Units;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoicePool;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};

    bool publish(const std::int16_t* pcm, std::uint32_t frames) {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = static_cast<std::size_t>(frames) * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = 0;
        record.descriptor.loop_end = frames;
        return directory.publish(0, &record);
    }
};

PlaybackZone liveZone() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.loop_mode = LoopMode::Continuous;
    zone.envelope.attack_frames = 0;
    zone.envelope.decay_frames = 0;
    zone.envelope.sustain_q15 = kQ15Unity;
    zone.envelope.release_frames = 4;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 0;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 960;
    zone.note_on_gain.pan_amount = 500;
    zone.note_on_gain.expression_to_attenuation_cb = 960;
    zone.note_on_gain.pan_sf2_units = 0;
    zone.note_on_gain.fixed_velocity = -1;
    zone.effects_send.enabled = true;
    zone.effects_send.reverb_sf2_units = 100;
    zone.effects_send.chorus_sf2_units = 50;
    zone.effects_send.reverb_cc_amount = 200;
    zone.effects_send.chorus_cc_amount = 300;
    return zone;
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

RealtimeEvent cc(std::uint8_t controller, std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = 2;
    event.data1 = controller;
    event.data2 = value;
    return event;
}

RealtimeEvent noteOn(std::uint8_t key) {
    RealtimeEvent event{};
    event.type = EventType::NoteOn;
    event.channel = 2;
    event.data1 = key;
    event.data2 = 100;
    return event;
}

RealtimeEvent noteOff(std::uint8_t key) {
    RealtimeEvent event{};
    event.type = EventType::NoteOff;
    event.channel = 2;
    event.data1 = key;
    return event;
}

RealtimeEvent pitchBend(std::uint16_t value) {
    RealtimeEvent event{};
    event.type = EventType::PitchBend;
    event.channel = 2;
    event.value = value;
    return event;
}

struct VoiceSnapshot {
    bool seen = false;
    std::uint64_t phase_increment_q32 = 0;
    std::int32_t gain_left_q15 = 0;
    std::int32_t gain_right_q15 = 0;
    std::int16_t reverb_send_units = 0;
    std::int16_t chorus_send_units = 0;
};

bool captureVoice(void* opaque, Voice& voice) {
    auto* snapshot = static_cast<VoiceSnapshot*>(opaque);
    if (snapshot == nullptr || snapshot->seen) {
        return false;
    }
    snapshot->seen = true;
    snapshot->phase_increment_q32 = voice.phaseIncrementQ32();
    snapshot->gain_left_q15 = voice.gainLeftQ15();
    snapshot->gain_right_q15 = voice.gainRightQ15();
    snapshot->reverb_send_units = voice.reverbSendUnits();
    snapshot->chorus_send_units = voice.chorusSendUnits();
    return false;
}

VoiceSnapshot activeVoice(VoicePool& voices) {
    VoiceSnapshot snapshot{};
    voices.updateActiveVoices(2, &captureVoice, &snapshot);
    assert(snapshot.seen);
    return snapshot;
}

struct RuntimeFixture {
    static constexpr std::array<std::int16_t, 16> pcm{
        12000, 12000, 12000, 12000, 12000, 12000, 12000, 12000,
        12000, 12000, 12000, 12000, 12000, 12000, 12000, 12000,
    };

    PublishedFixture published{};
    PlaybackZone zone{liveZone()};
    PlaybackProgramSnapshot program{programFor(&zone)};
    PlaybackProgramSnapshotExchange programs{};
    VoicePool voices{published.directory};
    RealtimeProgramEventApplier applier{programs};

    RuntimeFixture() {
        assert(published.publish(pcm.data(), pcm.size()));
        assert(program.valid());
        assert(programs.setInitialActive(&program));
    }
};

void testLiveGainPanAndPitchUpdateAlreadySoundingVoice() {
    RuntimeFixture f;
    assert(f.applier.apply(cc(7, 127), f.voices));
    assert(f.applier.apply(cc(11, 127), f.voices));
    assert(f.applier.apply(cc(10, 64), f.voices));
    assert(f.applier.apply(noteOn(60), f.voices));
    assert(f.voices.activeVoiceCount() == 1);

    VoiceSnapshot snapshot = activeVoice(f.voices);
    assert(snapshot.phase_increment_q32 == kPhaseOneQ32);
    assert(snapshot.gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(snapshot.gain_right_q15 == sf2PanChannelGainQ15(0, false));

    assert(f.applier.apply(cc(7, 80), f.voices));
    const VoiceSnapshot volume = activeVoice(f.voices);
    assert(volume.gain_left_q15 < snapshot.gain_left_q15);
    assert(volume.gain_left_q15 == volume.gain_right_q15);

    assert(f.applier.apply(cc(11, 90), f.voices));
    const VoiceSnapshot expression = activeVoice(f.voices);
    assert(expression.gain_left_q15 < volume.gain_left_q15);

    assert(f.applier.apply(cc(7, 127), f.voices));
    assert(f.applier.apply(cc(11, 127), f.voices));
    assert(f.applier.apply(cc(10, 0), f.voices));
    const VoiceSnapshot left = activeVoice(f.voices);
    assert(left.gain_left_q15 == kQ15Unity);
    assert(left.gain_right_q15 == 0);

    assert(f.applier.apply(cc(10, 64), f.voices));
    const VoiceSnapshot recentered = activeVoice(f.voices);
    assert(recentered.gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(recentered.gain_right_q15 == sf2PanChannelGainQ15(0, false));
    assert(f.applier.apply(pitchBend(16383), f.voices));
    const VoiceSnapshot bent = activeVoice(f.voices);
    assert(bent.phase_increment_q32 == ZoneResolver::transposeSemitones(kPhaseOneQ32, 2));

    assert(f.applier.apply(cc(101, 0), f.voices));
    assert(f.applier.apply(cc(100, 0), f.voices));
    assert(f.applier.apply(cc(6, 12), f.voices));
    const VoiceSnapshot wide_bend = activeVoice(f.voices);
    assert(wide_bend.phase_increment_q32 == ZoneResolver::transposeSemitones(kPhaseOneQ32, 12));
    assert(f.applier.stats().active_voice_updates >= 7);
}

void testCc91AndCc93UpdatePreparedEffectsSendState() {
    RuntimeFixture f;
    assert(f.applier.apply(noteOn(60), f.voices));
    VoiceSnapshot snapshot = activeVoice(f.voices);
    assert(snapshot.reverb_send_units == 100);
    assert(snapshot.chorus_send_units == 50);

    assert(f.applier.apply(cc(91, 64), f.voices));
    snapshot = activeVoice(f.voices);
    assert(snapshot.reverb_send_units ==
           channelEffectsSendSf2Units(100, 64, 200));
    assert(snapshot.chorus_send_units == 50);

    assert(f.applier.apply(cc(93, 127), f.voices));
    snapshot = activeVoice(f.voices);
    assert(snapshot.reverb_send_units ==
           channelEffectsSendSf2Units(100, 64, 200));
    assert(snapshot.chorus_send_units ==
           channelEffectsSendSf2Units(50, 127, 300));
    assert(f.applier.channelState(2).reverb_send == 64);
    assert(f.applier.channelState(2).chorus_send == 127);
}

void testSustainDefersNoteOffUntilPedalRelease() {
    RuntimeFixture f;
    assert(f.applier.apply(cc(7, 127), f.voices));
    assert(f.applier.apply(noteOn(60), f.voices));
    assert(f.applier.apply(cc(64, 127), f.voices));
    assert(f.applier.apply(noteOff(60), f.voices));
    assert(f.voices.activeVoiceCount() == 1);

    std::array<std::int32_t, 16> held{};
    f.voices.renderAdd(held.data(), held.size() / 2u);
    assert(f.voices.activeVoiceCount() == 1);

    assert(f.applier.apply(cc(64, 0), f.voices));
    assert(f.applier.stats().sustain_releases == 1);
    std::array<std::int32_t, 12> release{};
    f.voices.renderAdd(release.data(), release.size() / 2u);
    assert(f.voices.activeVoiceCount() == 0);
    assert(f.voices.stats().sustain_deferred_note_offs == 1);
    assert(f.voices.stats().sustain_release_voices == 1);
}

void testAllNotesOffRespectsSustainAndAllSoundOffIsImmediate() {
    RuntimeFixture f;
    assert(f.applier.apply(cc(7, 127), f.voices));
    assert(f.applier.apply(noteOn(60), f.voices));
    assert(f.applier.apply(noteOn(64), f.voices));
    assert(f.voices.activeVoiceCount() == 2);

    assert(f.applier.apply(cc(64, 127), f.voices));
    assert(f.applier.apply(cc(123, 0), f.voices));
    assert(f.voices.activeVoiceCount() == 2);
    assert(f.voices.stats().all_notes_off_matches == 2);

    assert(f.applier.apply(cc(120, 0), f.voices));
    assert(f.voices.activeVoiceCount() == 0);
    assert(f.applier.stats().all_notes_off_events == 1);
    assert(f.applier.stats().all_sound_off_events == 1);
    assert(f.voices.stats().channel_sound_off_voices == 2);
}

void testResetControllersReleasesSustainAndRecentersActivePitch() {
    RuntimeFixture f;
    assert(f.applier.apply(cc(7, 80), f.voices));
    assert(f.applier.apply(cc(10, 32), f.voices));
    assert(f.applier.apply(cc(64, 127), f.voices));
    assert(f.applier.apply(pitchBend(16383), f.voices));
    assert(f.applier.apply(noteOn(60), f.voices));
    assert(f.applier.apply(noteOff(60), f.voices));

    const VoiceSnapshot before = activeVoice(f.voices);
    assert(before.phase_increment_q32 == ZoneResolver::transposeSemitones(kPhaseOneQ32, 2));

    assert(f.applier.apply(cc(121, 0), f.voices));
    const auto& state = f.applier.channelState(2);
    assert(state.volume == 80);
    assert(state.pan == 32);
    assert(state.expression == 127);
    assert(!state.sustainDown());
    assert(state.pitch_bend == state.kPitchBendCenter);
    assert(state.rpnNull());

    const VoiceSnapshot after = activeVoice(f.voices);
    assert(after.phase_increment_q32 == kPhaseOneQ32);
    assert(after.gain_left_q15 > after.gain_right_q15);
    assert(after.gain_right_q15 > 0);
    // CC121 preserves channel effect-send mix state and therefore the sounding
    // Voice send state too; full GM reset is the operation that zeros CC91/93.
    assert(after.reverb_send_units == before.reverb_send_units);
    assert(after.chorus_send_units == before.chorus_send_units);
    assert(f.applier.stats().sustain_releases == 1);

    std::array<std::int32_t, 12> release{};
    f.voices.renderAdd(release.data(), release.size() / 2u);
    assert(f.voices.activeVoiceCount() == 0);
}

}  // namespace

int main() {
    testLiveGainPanAndPitchUpdateAlreadySoundingVoice();
    testCc91AndCc93UpdatePreparedEffectsSendState();
    testSustainDefersNoteOffUntilPedalRelease();
    testAllNotesOffRespectsSustainAndAllSoundOffIsImmediate();
    testResetControllersReleasesSustainAndRecentersActivePitch();
    std::cout << "GM active-voice channel control checks passed\n";
    return 0;
}