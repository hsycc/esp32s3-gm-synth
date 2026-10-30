#include "voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#if !GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
#error this differential suite requires the explicit experimental build
#endif

namespace {
namespace v = gm_synth::voice;
namespace s = gm_synth::sample;
namespace x = v::experimental;
constexpr auto Q = v::kPhaseOneQ32;

struct Fixture {
    s::PublishedSampleDirectory directory;
    s::ResidentRecord record{};
    std::array<std::int16_t, 1024> pcm{};
    x::LoopPlanContext context{};
    x::PinnedLoopPlan plan;
    v::Voice reference, realtime;

    Fixture(v::VoiceStartParams params, unsigned fade) {
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::int16_t>(static_cast<int>((i * 977u + 173u) % 60001u) - 30000);
        record.bank_generation = 1; record.sample_generation = 1; record.pcm = pcm.data();
        record.descriptor.id = 17; record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100; record.descriptor.flags = 1;
        record.descriptor.loop_start = 23; record.descriptor.loop_end = 923;
        assert(directory.publish(17, &record));
        params.sample_id = 17;
        params.sample_region = {true, 11, 1001, 23, 923};
        if (fade) {
            context.asset_sha256.fill(0x73); context.session_serial = 7;
            context.bank_generation = context.sample_generation = 1; context.sample_id = 17;
            context.sample_frames = pcm.size(); context.sample_rate = 44100;
            context.region = {11, 1001, 23, 923}; context.noteon_addresses_fixed = true;
            context.mode = params.loop_mode == v::LoopMode::Continuous
                ? x::CursorLoopMode::Continuous : x::CursorLoopMode::UntilRelease;
            x::LoopPlanRequest request;
            request.context = context; request.experimental_opt_in = true;
            request.fade = {true, static_cast<std::uint16_t>(fade)};
            request.release = {true, 32};
            assert(plan.bind(directory, request, context) == x::LoopBindingStatus::Ok);
            params.experimental_loop_opt_in = true;
            params.experimental_loop_plan = &plan;
            params.experimental_loop_context = &context;
        }
        assert(reference.start(directory, params));
        assert(realtime.start(directory, params));
        // The plan/program may retire immediately. Both Voices retain their
        // own pin; the optimized path must never borrow the expired plan.
        assert(plan.reset());
        assert(directory.unpublish(17) == &record);
        assert(!directory.canReclaim(&record));
    }
    ~Fixture() {
        reference.reset(); realtime.reset();
        assert(directory.stats().live_voice_refs == 0);
        assert(directory.stats().stale_release_faults == 0);
        assert(directory.canReclaim(&record));
    }
    void state() const {
        assert(reference.active() == realtime.active());
        assert(reference.releasing() == realtime.releasing());
        assert(reference.stealTail() == realtime.stealTail());
        assert(reference.exclusiveTail() == realtime.exclusiveTail());
        assert(reference.phaseQ32() == realtime.phaseQ32());
        assert(reference.envelopeLevelQ15() == realtime.envelopeLevelQ15());
        assert(reference.modEnvelopeLevelQ15() == realtime.modEnvelopeLevelQ15());
        assert(reference.currentPhaseIncrementQ32() == realtime.currentPhaseIncrementQ32());
        assert(reference.currentGainLeftQ15() == realtime.currentGainLeftQ15());
        assert(reference.currentGainRightQ15() == realtime.currentGainRightQ15());
        const auto& a = reference.currentLowpassConfig();
        const auto& b = realtime.currentLowpassConfig();
        assert(a.enabled == b.enabled && a.a0_q24 == b.a0_q24 && a.a1_q24 == b.a1_q24 &&
               a.b1_q24 == b.b1_q24 && a.b2_q24 == b.b2_q24);
        assert(reference.experimentalLoopEnabled() == realtime.experimentalLoopEnabled());
        assert(reference.experimentalLoopTransitionActive() == realtime.experimentalLoopTransitionActive());
    }
    void render(std::size_t frames) {
        std::vector<std::int32_t> a(frames * 2, 17), b(frames * 2, 17);
        const auto ar = reference.renderAdd(a.data(), frames);
        const auto br = realtime.renderAddRealtime(b.data(), frames);
        assert(a == b);
        assert(ar.frames_rendered == br.frames_rendered && ar.finished == br.finished);
        state();
    }
    void update(std::uint64_t step, int vibrato, bool unequal) {
        v::VoiceRuntimeControlUpdate update;
        update.phase_increment_q32 = step;
        update.gain_left_q15 = 12001; update.gain_right_q15 = unequal ? 19003 : 12001;
        update.vibrato_pitch_depth_cents = vibrato;
        update.cc21_filter_offset_cents = -300;
        assert(reference.applyRuntimeControlUpdate(update) == realtime.applyRuntimeControlUpdate(update));
        state();
    }
    void off(unsigned kind) {
        if (kind == 0) { reference.noteOff(); realtime.noteOff(); }
        else if (kind == 1)
            assert(reference.beginFastRelease(17) == realtime.beginFastRelease(17));
        else assert(reference.beginExclusiveRelease(31) == realtime.beginExclusiveRelease(31));
        state();
    }
};

v::VoiceStartParams params(unsigned filter, std::uint32_t delay, v::EnvelopeConfig envelope) {
    v::VoiceStartParams result;
    result.channel = 2; result.key = 60; result.velocity = 64;
    result.loop_mode = v::LoopMode::Continuous;
    result.phase_increment_q32 = Q + Q / 3u;
    result.gain_left_q15 = result.gain_right_q15 = 3630;
    result.envelope = envelope;
    result.vibrato_lfo = {true, delay, 19000001, 0};
    result.realtime_control.enabled = true;
    if (filter == 1) result.lowpass = {true, 1 << 21, 1 << 22, -(1 << 23), 1 << 21};
    // Stable feedback, with output beyond int16 on some inputs: the new path
    // must retain the reference's wide scaling and symmetric filter rounding.
    if (filter == 2) result.lowpass = {true, 1 << 24, 0, -(1 << 23), 0};
    return result;
}

void static_matrix() {
    const std::array<v::EnvelopeConfig, 5> envelopes{{
        {43, 44, 43, 43, 32767, 5291}, // actual Program16 at velocity64
        {0, 0, 0, 0, 23000, 37},
        {1, 2, 1, 3, 1, 17},
        {3, 19, 5, 61, 0, 1},
        {0, 1, 0, 1, 32767, 0},
    }};
    unsigned cases = 0;
    for (unsigned fade : {0u, 16u, 32u, 64u})
    for (unsigned filter : {0u, 1u, 2u})
    for (const auto& envelope : envelopes)
    for (std::uint32_t delay : {0u, 1u, 16u, 43u, 113u}) {
        auto p = params(filter, delay, envelope);
        p.phase_increment_q32 = cases % 3 == 0 ? Q / 2u : cases % 3 == 1 ? Q : Q + Q / 3u;
        if (cases % 2) p.gain_right_q15 = 17003;
        Fixture fixture(p, fade);
        fixture.render(0);
        for (unsigned frames : {1u, 15u, 1u, 26u, 1u, 17u, 3u, 65u, 7u}) fixture.render(frames);
        // Oscillator phase/control state is observed through subsequent audible
        // modulation, including entry to and exit from the static specialization.
        fixture.update(Q * 5u / 4u, 120, true);
        for (unsigned frames : {1u, 3u, 19u, 31u, 64u}) fixture.render(frames);
        fixture.update(Q * 3u / 4u, 0, false);
        for (unsigned frames : {1u, 17u, 64u, 129u, 3u}) fixture.render(frames);
        fixture.update(Q + Q / 5u, -150, true);
        fixture.render(97);
        fixture.update(Q + Q / 5u, 0, true);
        fixture.off(cases % 3u);
        fixture.off(cases % 3u);
        for (unsigned frames : {1u, 17u, 64u, 3u, 127u, 6001u}) fixture.render(frames);
        assert(!fixture.reference.active());
        ++cases;
    }
    assert(cases == 300);
    std::cout << "Static DSP PCM/state/controller matrix: " << cases << " cases PASS\n";
}

void oscillator_boundaries() {
    unsigned cases = 0;
    for (unsigned fade : {0u, 32u})
    for (std::uint32_t delay : {0u, 1u, 15u, 16u, 17u, 43u, 128u})
    for (unsigned first : {1u, 15u, 16u, 17u, 43u, 64u, 129u}) {
        auto p = params(1, delay, {0, 0, 0, 0, 19000, 71});
        Fixture fixture(p, fade);
        fixture.render(first);
        fixture.update(Q, 300, false);
        for (unsigned frames : {1u, 1u, 7u, 13u, 31u, 64u}) fixture.render(frames);
        fixture.update(Q, 0, false);
        fixture.render(3); fixture.render(17); // leave a nonzero control countdown
        fixture.update(Q * 2u, 350, true);
        fixture.render(193);
        ++cases;
    }
    std::cout << "Zero-depth vibrato delay/tick transitions: " << cases << " cases PASS\n";
}

void envelope_extremes_and_steps() {
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    unsigned cases = 0;
    for (unsigned fade : {0u, 32u})
    for (auto duration : {maximum, maximum - 1u, 131077u})
    for (unsigned stage : {0u, 1u, 2u}) {
        v::EnvelopeConfig envelope{0, 0, 0, 0, stage == 1 ? 1 : 32767, duration};
        if (stage == 0) envelope.attack_frames = duration;
        if (stage == 1) envelope.decay_frames = duration;
        Fixture fixture(params(1, 43, envelope), fade);
        fixture.render(1);
        if (stage == 2) fixture.off(0);
        // Cross both uint32 numerator range and remainder-borrow boundaries.
        for (unsigned frames : {131071u, 7u, 65u, 17u}) fixture.render(frames);
        fixture.update(Q, 150, true); fixture.render(127);
        ++cases;
    }
    for (unsigned fade : {0u, 16u, 32u, 64u})
    for (auto step : {std::uint64_t{1}, Q / 2u, Q, Q * 1903u + Q / 7u,
                      std::numeric_limits<std::uint64_t>::max()}) {
        auto p = params(2, 0, {0, 0, 0, 0, 27000, 67});
        p.phase_increment_q32 = step;
        Fixture fixture(p, fade);
        for (unsigned frames : {1u, 64u, 3u, 129u, 17u}) fixture.render(frames);
        fixture.off(0); fixture.render(1); fixture.render(127);
        ++cases;
    }
    std::cout << "Envelope integer bounds and extreme transport steps: " << cases << " cases PASS\n";
}

void dynamic_and_until_release_fallback() {
    unsigned cases = 0;
    for (unsigned fade : {0u, 32u})
    for (unsigned profile : {0u, 1u, 2u, 3u}) {
        auto p = params(1, 7, {3, 19, 7, 37, 18000, 211});
        if (profile == 0) p.loop_mode = v::LoopMode::UntilRelease;
        if (profile == 1) {
            p.mod_lfo.enabled = true; p.mod_lfo.phase_step_q32 = 17000000;
            p.mod_lfo.pitch_depth_cents = 300;
        }
        if (profile == 2) {
            p.mod_envelope.enabled = true;
            p.mod_envelope.envelope = {3, 21, 7, 35, 15000, 79};
            p.mod_envelope.pitch_depth_cents = -120;
        }
        if (profile == 3) {
            p.dynamic_lowpass_table.enabled = true;
            p.dynamic_lowpass_table.min_offset_cents = -400;
            p.dynamic_lowpass_table.max_offset_cents = 400;
            for (auto& coefficient : p.dynamic_lowpass_table.points) coefficient = p.lowpass;
        }
        Fixture fixture(p, fade);
        for (unsigned frames : {1u, 17u, 64u, 127u, 733u}) fixture.render(frames);
        fixture.update(Q * 3u / 2u, 300, true);
        fixture.render(17);
        assert(fixture.reference.applyCc21FilterOffset(300) == fixture.realtime.applyCc21FilterOffset(300));
        fixture.render(64);
        fixture.off(0);
        for (unsigned frames : {1u, 17u, 64u, 511u}) fixture.render(frames);
        assert(!fixture.reference.active());
        ++cases;
    }
    std::cout << "Dynamic modulation and UntilRelease fallback: " << cases << " cases PASS\n";
}

} // namespace

int main() {
    static_matrix();
    oscillator_boundaries();
    envelope_extremes_and_steps();
    dynamic_and_until_release_fallback();
    std::cout << "Loop Voice realtime fast-path differential qualification PASS physical_timing_approved=0\n";
}
