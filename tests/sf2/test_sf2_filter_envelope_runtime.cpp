#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/gm/ZoneResolver.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"
#include "../../firmware/sf2/Sf2FilterMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::prepareLowpassQ24;
using gm_synth::sf2::velocityToFilterCutoffCents;
using gm_synth::voice::BiquadLowpassConfig;
using gm_synth::voice::Voice;
using gm_synth::voice::VoiceStartParams;
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

void testVelocityToFilterCutoffUsesSf204LinearSource() {
    assert(velocityToFilterCutoffCents(127, -2400) == 0);
    assert(velocityToFilterCutoffCents(64, -2400) == -1200);
    assert(velocityToFilterCutoffCents(1, -2400) == -2381);
    assert(velocityToFilterCutoffCents(64, 0) == 0);
}

void testLowpassCoefficientPreparationIsBoundedAndExplicit() {
    BiquadLowpassConfig high{};
    BiquadLowpassConfig low{};
    assert(prepareLowpassQ24(13500, 0, 44100, &high));
    assert(prepareLowpassQ24(6900, 0, 44100, &low));
    assert(high.enabled && high.valid());
    assert(low.enabled && low.valid());
    assert(high.a0_q24 > low.a0_q24);
    assert(high.a1_q24 > low.a1_q24);

    BiquadLowpassConfig rejected{};
    assert(!prepareLowpassQ24(6900, 60, 44100, &rejected));
    assert(!prepareLowpassQ24(6900, 0, 48000, &rejected));
}

void testVoiceActuallyProcessesPreparedLowpass() {
    constexpr std::array<std::int16_t, 16> impulse{
        30000, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    PublishedFixture fixture;
    assert(fixture.publish(impulse.data(), impulse.size()));

    VoiceStartParams dry{};
    dry.sample_id = 0;
    dry.phase_increment_q32 = kPhaseOneQ32;
    dry.gain_left_q15 = kQ15Unity;
    dry.gain_right_q15 = kQ15Unity;
    dry.envelope.release_frames = 1;

    Voice dry_voice;
    assert(dry_voice.start(fixture.directory, dry));
    std::array<std::int32_t, 8> dry_output{};
    const auto dry_result = dry_voice.renderAdd(dry_output.data(), dry_output.size() / 2u);
    assert(dry_result.frames_rendered == 4);
    assert(dry_output[0] == 30000);
    dry_voice.reset();

    VoiceStartParams filtered = dry;
    assert(prepareLowpassQ24(6900, 0, 44100, &filtered.lowpass));
    Voice filtered_voice;
    assert(filtered_voice.start(fixture.directory, filtered));
    std::array<std::int32_t, 8> filtered_output{};
    const auto filtered_result = filtered_voice.renderAdd(
        filtered_output.data(),
        filtered_output.size() / 2u);
    assert(filtered_result.frames_rendered == 4);
    assert(filtered_output[0] > 0);
    assert(filtered_output[0] < dry_output[0]);
    assert(filtered_output[2] != 0 || filtered_output[4] != 0);
}

void testDelayAttackHoldDecayStagesRenderInOrder() {
    constexpr std::array<std::int16_t, 16> constant{
        10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000,
        10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000,
    };
    PublishedFixture fixture;
    assert(fixture.publish(constant.data(), constant.size()));

    VoiceStartParams params{};
    params.sample_id = 0;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.envelope.delay_frames = 2;
    params.envelope.attack_frames = 2;
    params.envelope.hold_frames = 2;
    params.envelope.decay_frames = 2;
    params.envelope.sustain_q15 = kQ15Unity / 2;
    params.envelope.release_frames = 2;

    Voice voice;
    assert(voice.start(fixture.directory, params));
    std::array<std::int32_t, 16> output{};
    const auto result = voice.renderAdd(output.data(), output.size() / 2u);
    assert(result.frames_rendered == 8);

    // Left channel: delay, attack, hold, then two-frame decay to half sustain.
    assert(output[0] == 0);
    assert(output[2] == 0);
    assert(output[4] > 4500 && output[4] < 5500);
    assert(output[6] > 9500);
    assert(output[8] > 9500);
    assert(output[10] > 9500);
    assert(output[12] > 7000 && output[12] < 8000);
    assert(output[14] > 4500 && output[14] < 5500);
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

void testResolverAppliesKeyScaledEnvelopeAndEffectiveVelocityFilter() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.fixed_velocity = 64;
    zone.note_on_filter.enabled = true;
    zone.note_on_filter.initial_cutoff_cents = 9000;
    zone.note_on_filter.velocity_to_cutoff_cents = -2400;
    zone.note_on_filter.resonance_cb = 0;
    zone.envelope.hold_frames = 100;
    zone.envelope.decay_frames = 100;
    zone.envelope_key_scaling.hold_timecents_per_key = 100;
    zone.envelope_key_scaling.decay_timecents_per_key = 100;

    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 72, 127, 1, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].velocity == 64);
    assert(batch.voices[0].envelope.hold_frames == 50);
    assert(batch.voices[0].envelope.decay_frames == 50);
    assert(batch.voices[0].lowpass.enabled);

    BiquadLowpassConfig expected{};
    assert(prepareLowpassQ24(7800, 0, 44100, &expected));
    assert(batch.voices[0].lowpass.a0_q24 == expected.a0_q24);
    assert(batch.voices[0].lowpass.a1_q24 == expected.a1_q24);
    assert(batch.voices[0].lowpass.b1_q24 == expected.b1_q24);
    assert(batch.voices[0].lowpass.b2_q24 == expected.b2_q24);

    assert(resolver.resolveNoteOn(program, 2, 48, 127, 2, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].envelope.hold_frames == 200);
    assert(batch.voices[0].envelope.decay_frames == 200);
}

}  // namespace

int main() {
    testVelocityToFilterCutoffUsesSf204LinearSource();
    testLowpassCoefficientPreparationIsBoundedAndExplicit();
    testVoiceActuallyProcessesPreparedLowpass();
    testDelayAttackHoldDecayStagesRenderInOrder();
    testResolverAppliesKeyScaledEnvelopeAndEffectiveVelocityFilter();
    std::cout << "SF2 filter/envelope runtime checks passed\n";
    return 0;
}
