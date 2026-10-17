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
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueFilterPending;
using gm_synth::sf2::PlaybackIssueModEnvelopePending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

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
    zone.sample_end = 512;
    zone.loop_start = 0;
    zone.loop_end = 512;
    zone.shdr_start = 0;
    zone.shdr_end = 512;
    zone.shdr_loop_start = 0;
    zone.shdr_loop_end = 512;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    zone.generators.set(54, 1);
    return zone;
}

void testMaterializerCarriesFullModEnvelope() {
    NativeResolvedZone native = baseNativeZone();
    native.generators.set(7, 120);     // modEnvToPitch
    native.generators.set(11, 1200);   // modEnvToFilterFc
    native.generators.set(25, -1200);  // delay = 0.5 s
    native.generators.set(26, 0);      // attack = 1 s
    native.generators.set(27, 1200);   // hold = 2 s
    native.generators.set(28, -1200);  // decay = 0.5 s
    native.generators.set(29, 250);    // sustain = 75%
    native.generators.set(30, 0);      // release = 1 s
    native.generators.set(31, 100);
    native.generators.set(32, -50);

    PlaybackZone zone{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(
        native,
        ResolvedZoneModulation{},
        &zone);

    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueModEnvelopePending));
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(zone.mod_envelope.enabled);
    assert(zone.mod_envelope.pitch_depth_cents == 120);
    assert(zone.mod_envelope.filter_depth_cents == 1200);
    assert(zone.mod_envelope.envelope.delay_frames == 22050);
    assert(zone.mod_envelope.envelope.attack_frames == 44100);
    assert(zone.mod_envelope.envelope.hold_frames == 88200);
    assert(zone.mod_envelope.envelope.decay_frames == 22050);
    assert(zone.mod_envelope.envelope.sustain_q15 == 24575);
    assert(zone.mod_envelope.envelope.release_frames == 44100);
    assert(zone.mod_envelope.key_scaling.hold_timecents_per_key == 100);
    assert(zone.mod_envelope.key_scaling.decay_timecents_per_key == -50);
    assert(zone.valid());
}

PlaybackZone baseRuntimeZone() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.loop_mode = LoopMode::Continuous;

    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 0;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 0;
    zone.note_on_gain.pan_amount = 0;
    zone.note_on_gain.expression_to_attenuation_cb = 0;
    zone.note_on_gain.pan_sf2_units = 0;

    zone.note_on_filter.enabled = true;
    zone.note_on_filter.initial_cutoff_cents = 8000;
    zone.note_on_filter.velocity_to_cutoff_cents = 0;
    zone.note_on_filter.resonance_cb = 0;

    zone.envelope.delay_frames = 0;
    zone.envelope.attack_frames = 0;
    zone.envelope.hold_frames = 0;
    zone.envelope.decay_frames = 0;
    zone.envelope.sustain_q15 = kQ15Unity;
    zone.envelope.release_frames = 256;
    return zone;
}

PlaybackProgramSnapshot programFor(const PlaybackZone* zone) {
    PlaybackProgramSnapshot program{};
    program.channel = 3;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = 1;
    program.zones = zone;
    program.zone_count = 1;
    return program;
}

MidiChannelState neutralChannelState() {
    MidiChannelState state{};
    state.resetAll();
    state.volume = 127;
    state.expression = 127;
    state.pan = 64;
    return state;
}

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 512> pcm{};

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

ResolvedVoiceBatch resolve(
    const PlaybackZone& zone,
    std::uint8_t key,
    std::uint32_t note_instance_id) {
    const PlaybackProgramSnapshot program = programFor(&zone);
    MidiChannelState state = neutralChannelState();
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(
               program,
               state,
               3,
               key,
               100,
               note_instance_id,
               &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    return batch;
}

void testKeynumScalesModEnvelopeHoldAndDecay() {
    PlaybackZone zone = baseRuntimeZone();
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.pitch_depth_cents = 100;
    zone.mod_envelope.envelope.hold_frames = 64;
    zone.mod_envelope.envelope.decay_frames = 64;
    zone.mod_envelope.envelope.sustain_q15 = kQ15Unity / 2;
    zone.mod_envelope.envelope.release_frames = 64;
    zone.mod_envelope.key_scaling.hold_timecents_per_key = 100;
    zone.mod_envelope.key_scaling.decay_timecents_per_key = 100;

    const ResolvedVoiceBatch high = resolve(zone, 72, 1);
    assert(high.voices[0].mod_envelope.envelope.hold_frames >= 31);
    assert(high.voices[0].mod_envelope.envelope.hold_frames <= 33);
    assert(high.voices[0].mod_envelope.envelope.decay_frames >= 31);
    assert(high.voices[0].mod_envelope.envelope.decay_frames <= 33);

    const ResolvedVoiceBatch low = resolve(zone, 48, 2);
    assert(low.voices[0].mod_envelope.envelope.hold_frames >= 127);
    assert(low.voices[0].mod_envelope.envelope.hold_frames <= 129);
    assert(low.voices[0].mod_envelope.envelope.decay_frames >= 127);
    assert(low.voices[0].mod_envelope.envelope.decay_frames <= 129);
}

void testConvexAttackDrivesPitchAndFilter() {
    PublishedFixture fixture;
    PlaybackZone zone = baseRuntimeZone();
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.pitch_depth_cents = 1200;
    zone.mod_envelope.filter_depth_cents = 2400;
    zone.mod_envelope.envelope.attack_frames = 64;
    zone.mod_envelope.envelope.hold_frames = 0;
    zone.mod_envelope.envelope.decay_frames = 64;
    zone.mod_envelope.envelope.sustain_q15 = kQ15Unity / 2;
    zone.mod_envelope.envelope.release_frames = 64;

    const ResolvedVoiceBatch batch = resolve(zone, 60, 3);
    assert(batch.voices[0].mod_envelope.enabled);
    assert(batch.voices[0].dynamic_lowpass_table.enabled);

    Voice voice;
    assert(voice.start(fixture.directory, batch.voices[0]));
    const std::uint64_t base_phase = voice.phaseIncrementQ32();
    const std::int32_t base_filter_a0 = voice.lowpassConfig().a0_q24;

    std::array<std::int32_t, 2> first_frame{};
    assert(voice.renderAdd(first_frame.data(), 1).frames_rendered == 1);
    assert(voice.modEnvelopeLevelQ15() > 0);
    assert(voice.modEnvelopeDspLevelQ15() > voice.modEnvelopeLevelQ15());
    assert(voice.currentPhaseIncrementQ32() > base_phase);
    assert(voice.currentLowpassConfig().a0_q24 > base_filter_a0);

    std::array<std::int32_t, 126> rest_attack{};
    assert(voice.renderAdd(rest_attack.data(), 63).frames_rendered == 63);
    assert(voice.modEnvelopeDspLevelQ15() > kQ15Unity / 2);
    assert(voice.currentPhaseIncrementQ32() > base_phase);
    assert(voice.currentLowpassConfig().a0_q24 > base_filter_a0);
}

void testModLfoAndModEnvelopeFilterOffsetsAdd() {
    PublishedFixture fixture;
    PlaybackZone zone = baseRuntimeZone();
    zone.mod_lfo.enabled = true;
    zone.mod_lfo.phase_step_q32 = std::uint32_t{1} << 26u; // +1 at frame 16 tick
    zone.mod_lfo.filter_depth_cents = 600;
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.filter_depth_cents = 600;
    zone.mod_envelope.envelope.sustain_q15 = kQ15Unity;
    zone.mod_envelope.envelope.release_frames = 64;

    const ResolvedVoiceBatch batch = resolve(zone, 60, 4);
    assert(batch.voices[0].dynamic_lowpass_table.enabled);
    assert(batch.voices[0].dynamic_lowpass_table.min_offset_cents <= -600);
    assert(batch.voices[0].dynamic_lowpass_table.max_offset_cents >= 1200);

    Voice voice;
    assert(voice.start(fixture.directory, batch.voices[0]));

    std::array<std::int32_t, 2> first{};
    assert(voice.renderAdd(first.data(), 1).frames_rendered == 1);
    const std::int32_t envelope_only_a0 = voice.currentLowpassConfig().a0_q24;
    assert(envelope_only_a0 > voice.lowpassConfig().a0_q24);

    std::array<std::int32_t, 32> to_positive_peak{};
    assert(voice.renderAdd(to_positive_peak.data(), 16).frames_rendered == 16);
    assert(voice.currentLowpassConfig().a0_q24 > envelope_only_a0);
}

void testNoteOffUsesIndependentModEnvelopeRelease() {
    PublishedFixture fixture;
    PlaybackZone zone = baseRuntimeZone();
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.pitch_depth_cents = 1200;
    zone.mod_envelope.envelope.sustain_q15 = kQ15Unity;
    zone.mod_envelope.envelope.release_frames = 64;

    const ResolvedVoiceBatch batch = resolve(zone, 60, 5);
    Voice voice;
    assert(voice.start(fixture.directory, batch.voices[0]));

    std::array<std::int32_t, 2> first{};
    assert(voice.renderAdd(first.data(), 1).frames_rendered == 1);
    const std::uint64_t full_depth_phase = voice.currentPhaseIncrementQ32();
    assert(full_depth_phase > voice.phaseIncrementQ32());

    voice.noteOff();
    std::array<std::int32_t, 34> release{};
    assert(voice.renderAdd(release.data(), 17).frames_rendered == 17);
    assert(voice.modEnvelopeDspLevelQ15() < kQ15Unity);
    assert(voice.currentPhaseIncrementQ32() < full_depth_phase);
    assert(voice.currentPhaseIncrementQ32() > voice.phaseIncrementQ32());
}

}  // namespace

int main() {
    testMaterializerCarriesFullModEnvelope();
    testKeynumScalesModEnvelopeHoldAndDecay();
    testConvexAttackDrivesPitchAndFilter();
    testModLfoAndModEnvelopeFilterOffsetsAdd();
    testNoteOffUsesIndependentModEnvelopeRelease();
    std::cout << "SF2 modulation-envelope runtime checks passed\n";
    return 0;
}