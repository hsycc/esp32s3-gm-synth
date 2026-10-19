#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct Fixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};

    bool publish(const std::int16_t* pcm, std::uint32_t frames,
                 std::uint32_t loop_start, std::uint32_t loop_end) {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = frames * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = loop_start;
        record.descriptor.loop_end = loop_end;
        return directory.publish(0, &record);
    }
};

VoiceStartParams steadyParams() {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 1;
    params.key = 60;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32 + kPhaseOneQ32 / 3u;
    params.loop_mode = LoopMode::Continuous;
    params.gain_left_q15 = 25000;
    params.gain_right_q15 = 17000;
    params.envelope.attack_frames = 0;
    params.envelope.hold_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = 23000;
    params.envelope.release_frames = 8;
    return params;
}

void assertSameRenderState(
    const Voice& generic,
    const Voice& realtime,
    const gm_synth::voice::VoiceRenderResult& generic_result,
    const gm_synth::voice::VoiceRenderResult& realtime_result) {
    assert(generic_result.frames_rendered == realtime_result.frames_rendered);
    assert(generic_result.finished == realtime_result.finished);
    assert(generic.phaseQ32() == realtime.phaseQ32());
    assert(generic.envelopeLevelQ15() == realtime.envelopeLevelQ15());
    assert(generic.modEnvelopeLevelQ15() == realtime.modEnvelopeLevelQ15());
    assert(generic.currentPhaseIncrementQ32() == realtime.currentPhaseIncrementQ32());
    assert(generic.currentGainLeftQ15() == realtime.currentGainLeftQ15());
    assert(generic.currentGainRightQ15() == realtime.currentGainRightQ15());
    const auto& generic_filter = generic.currentLowpassConfig();
    const auto& realtime_filter = realtime.currentLowpassConfig();
    assert(generic_filter.enabled == realtime_filter.enabled);
    assert(generic_filter.a0_q24 == realtime_filter.a0_q24);
    assert(generic_filter.a1_q24 == realtime_filter.a1_q24);
    assert(generic_filter.b1_q24 == realtime_filter.b1_q24);
    assert(generic_filter.b2_q24 == realtime_filter.b2_q24);
}

void testSteadySustainFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        1200, -4000, 8000, -12000, 16000, -8000, 4000, -2000, 6000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    Voice generic;
    Voice realtime;
    const VoiceStartParams params = steadyParams();
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 48;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};

    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);
    assert(generic_fixture.directory.stats().live_voice_refs == 1);
    assert(realtime_fixture.directory.stats().live_voice_refs == 1);

    generic.reset();
    realtime.reset();
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

void testMultiLoopIncrementFallbackIsBitExact() {
    constexpr std::array<std::int16_t, 10> pcm{
        900, -1800, 3600, -7200, 14400, -11000, 7000, -3500, 1750, -800};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    // Seven-frame loop, but advance by more than two loop lengths per output
    // frame. This must take the modulo fallback rather than the common
    // single-wrap path and still match the generic reference exactly.
    params.phase_increment_q32 = 15u * kPhaseOneQ32 + kPhaseOneQ32 / 4u;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 32;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testQ15GainInt32ExtremesAreBitExact() {
    constexpr std::array<std::int16_t, 8> pcm{
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max(),
        -32767,
        32766,
        -1,
        1,
        -16384,
        16384,
    };

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 0, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 0, 8));

    VoiceStartParams params = steadyParams();
    params.phase_increment_q32 = kPhaseOneQ32;
    params.gain_left_q15 = kQ15Unity - 1;
    params.gain_right_q15 = kQ15Unity / 2;
    params.envelope.sustain_q15 = kQ15Unity;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 32;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testSimpleAttackDecayFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 8> pcm{
        1000, -2000, 4000, -8000, 12000, -6000, 3000, -1500};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 7));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 7));

    VoiceStartParams params = steadyParams();
    params.envelope.attack_frames = 5;
    params.envelope.decay_frames = 7;
    params.envelope.sustain_q15 = 19000;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 24;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testSimpleReleaseFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        -9000, -4500, 0, 4500, 9000, 4500, 0, -4500, -9000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    params.envelope.release_frames = 9;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));
    generic.noteOff();
    realtime.noteOff();

    constexpr std::size_t kFrames = 16;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);
    assert(generic_result.finished);
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

VoiceStartParams complexContinuousParams() {
    VoiceStartParams params = steadyParams();
    params.phase_increment_q32 = kPhaseOneQ32 + kPhaseOneQ32 / 5u;
    params.gain_left_q15 = 24000;
    params.gain_right_q15 = 21000;
    params.envelope.sustain_q15 = 26000;
    params.envelope.release_frames = 11;

    params.lowpass.enabled = true;
    params.lowpass.a0_q24 = std::int32_t{1} << 22;
    params.lowpass.a1_q24 = std::int32_t{1} << 21;
    params.lowpass.b1_q24 = 0;
    params.lowpass.b2_q24 = 0;

    params.dynamic_lowpass_table.enabled = true;
    params.dynamic_lowpass_table.min_offset_cents = -256;
    params.dynamic_lowpass_table.max_offset_cents = 256;
    for (std::size_t index = 0; index < params.dynamic_lowpass_table.points.size(); ++index) {
        auto& filter = params.dynamic_lowpass_table.points[index];
        filter.enabled = true;
        filter.a0_q24 = (std::int32_t{1} << 22) + static_cast<std::int32_t>(index * 8192u);
        filter.a1_q24 = (std::int32_t{1} << 21) + static_cast<std::int32_t>(index * 4096u);
        filter.b1_q24 = 0;
        filter.b2_q24 = 0;
    }

    params.vibrato_lfo.enabled = true;
    params.vibrato_lfo.delay_frames = 0;
    params.vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 27u;
    params.vibrato_lfo.pitch_depth_cents = 47;

    params.mod_lfo.enabled = true;
    params.mod_lfo.delay_frames = 0;
    params.mod_lfo.phase_step_q32 = std::uint32_t{1} << 26u;
    params.mod_lfo.pitch_depth_cents = 73;
    params.mod_lfo.filter_depth_cents = 144;
    params.mod_lfo.gain_table.enabled = true;
    for (std::size_t index = 0; index < params.mod_lfo.gain_table.points.size(); ++index) {
        params.mod_lfo.gain_table.points[index].left =
            18000 + static_cast<std::int32_t>(index * 900u);
        params.mod_lfo.gain_table.points[index].right =
            24500 - static_cast<std::int32_t>(index * 700u);
    }

    params.mod_envelope.enabled = true;
    params.mod_envelope.envelope.attack_frames = 0;
    params.mod_envelope.envelope.hold_frames = 0;
    params.mod_envelope.envelope.decay_frames = 0;
    params.mod_envelope.envelope.sustain_q15 = 17000;
    params.mod_envelope.envelope.release_frames = 9;
    params.mod_envelope.pitch_depth_cents = 81;
    params.mod_envelope.filter_depth_cents = 96;
    return params;
}

void testComplexContinuousDspFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 13> pcm{
        -12000, -7000, -2000, 3000, 8000, 13000, 9000,
        4000, -1000, -6000, -11000, -5000, 2000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 12));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 12));

    const VoiceStartParams params = complexContinuousParams();
    assert(params.lowpass.valid());
    assert(params.dynamic_lowpass_table.valid());
    assert(params.vibrato_lfo.valid());
    assert(params.mod_lfo.valid());
    assert(params.mod_envelope.valid());

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    // Cross multiple 16-frame modulation control ticks and many loop wraps.
    constexpr std::size_t kSustainFrames = 128;
    std::array<std::int32_t, kSustainFrames * 2> generic_mix{};
    std::array<std::int32_t, kSustainFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kSustainFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kSustainFrames);
    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);
    assert(!generic_result.finished);

    // Release also exercises the wide filtered-sample gain path and the
    // modulation-envelope release state while the loop remains continuous.
    generic.noteOff();
    realtime.noteOff();
    constexpr std::size_t kReleaseFrames = 16;
    std::array<std::int32_t, kReleaseFrames * 2> generic_release{};
    std::array<std::int32_t, kReleaseFrames * 2> realtime_release{};
    const auto generic_release_result = generic.renderAdd(
        generic_release.data(), kReleaseFrames);
    const auto realtime_release_result = realtime.renderAddRealtime(
        realtime_release.data(), kReleaseFrames);
    assert(generic_release == realtime_release);
    assertSameRenderState(
        generic,
        realtime,
        generic_release_result,
        realtime_release_result);
    assert(generic_release_result.finished);
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

VoiceStartParams heavyContinuousParams(bool wide_dynamic_span) {
    VoiceStartParams params = complexContinuousParams();
    params.phase_increment_q32 = kPhaseOneQ32 + kPhaseOneQ32 / 7u;
    params.envelope.sustain_q15 = kQ15Unity;
    params.envelope.release_frames = 4410;

    params.dynamic_lowpass_table.min_offset_cents = wide_dynamic_span ? -20000 : -1500;
    params.dynamic_lowpass_table.max_offset_cents = wide_dynamic_span ? 20000 : 1500;
    for (std::size_t index = 0; index < params.dynamic_lowpass_table.points.size(); ++index) {
        auto& filter = params.dynamic_lowpass_table.points[index];
        filter.enabled = true;
        filter.a0_q24 = (std::int32_t{1} << 22) + static_cast<std::int32_t>(index * 12288u);
        filter.a1_q24 = (std::int32_t{1} << 21) + static_cast<std::int32_t>(index * 6144u);
        filter.b1_q24 = 0;
        filter.b2_q24 = 0;
    }

    params.vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 19u;
    params.vibrato_lfo.pitch_depth_cents = 50;
    params.mod_lfo.phase_step_q32 = std::uint32_t{1} << 20u;
    params.mod_lfo.pitch_depth_cents = 100;
    params.mod_lfo.filter_depth_cents = 1200;
    params.mod_envelope.envelope.sustain_q15 = kQ15Unity / 2;
    params.mod_envelope.pitch_depth_cents = 600;
    params.mod_envelope.filter_depth_cents = 600;
    return params;
}

void runHeavyScaleComparison(bool wide_dynamic_span) {
    constexpr std::array<std::int16_t, 13> pcm{
        -12000, -7000, -2000, 3000, 8000, 13000, 9000,
        4000, -1000, -6000, -11000, -5000, 2000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 12));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 12));

    const VoiceStartParams params = heavyContinuousParams(wide_dynamic_span);
    assert(params.dynamic_lowpass_table.valid());
    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    // 4096 frames give the Mod-LFO one complete cycle. This covers positive
    // and negative Q15 scaling, every 16-frame control tick, the physical-heavy
    // pitch/filter depth range, and many continuous-loop wraps.
    constexpr std::size_t kBlockFrames = 64;
    constexpr std::size_t kBlocks = 64;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        std::array<std::int32_t, kBlockFrames * 2> generic_mix{};
        std::array<std::int32_t, kBlockFrames * 2> realtime_mix{};
        const auto generic_result = generic.renderAdd(generic_mix.data(), kBlockFrames);
        const auto realtime_result = realtime.renderAddRealtime(
            realtime_mix.data(), kBlockFrames);
        assert(generic_mix == realtime_mix);
        assertSameRenderState(generic, realtime, generic_result, realtime_result);
        assert(!generic_result.finished);
    }

    generic.reset();
    realtime.reset();
}

void testHeavyFixtureScaleRangesAreBitExact() {
    // 3000-cent dynamic span exercises the common 32-bit ratio fast path.
    runHeavyScaleComparison(false);
}

void testWideDynamicFilterSpanFallbackIsBitExact() {
    // 40000 cents is intentionally wider than the safe direct-product limit,
    // forcing the exact binary-ratio fallback with no 64-bit divide.
    runHeavyScaleComparison(true);
}

void testUntilReleaseModeFallsBackBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        700, -1400, 2800, -5600, 11200, -5600, 2800, -1400, 700};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    params.loop_mode = LoopMode::UntilRelease;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 24;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

}  // namespace

int main() {
    testSteadySustainFastPathIsBitExact();
    testMultiLoopIncrementFallbackIsBitExact();
    testQ15GainInt32ExtremesAreBitExact();
    testSimpleAttackDecayFastPathIsBitExact();
    testSimpleReleaseFastPathIsBitExact();
    testComplexContinuousDspFastPathIsBitExact();
    testHeavyFixtureScaleRangesAreBitExact();
    testWideDynamicFilterSpanFallbackIsBitExact();
    testUntilReleaseModeFallsBackBitExact();
    std::cout << "Voice realtime fast-path checks passed\n";
    return 0;
}
