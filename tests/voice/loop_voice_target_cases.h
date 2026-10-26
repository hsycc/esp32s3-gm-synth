#pragma once

#include "voice/VoiceRenderer.h"
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP != 1
#error This qualification fixture requires the enabled production Voice layout.
#endif

namespace loop_target_test {
namespace v = gm_synth::voice;
namespace s = gm_synth::sample;
namespace x = v::experimental;
constexpr auto Q = v::kPhaseOneQ32;

// Checks remain active even when ESP-IDF defines NDEBUG. Large buffers and
// directories are static TEST storage, not allocations in the audio path.
inline void require(bool ok, unsigned line) {
    if (!ok) {
        std::printf("V1_TARGET_FAIL line=%u\n", line);
        std::fflush(stdout);
        std::abort();
    }
}
#define V1_REQUIRE(test) ::loop_target_test::require(bool(test), __LINE__)

inline void run(void (*yield_between_cases)() = nullptr) {
    static s::PublishedSampleDirectory directory, offline;
    static std::array<std::int16_t, 603> pcm{};
    static std::array<std::int16_t, 2048> stream{};
    static s::ResidentRecord record{}, stream_record{};
    static v::Voice generic, realtime, reference, rejected;
    static std::array<std::int32_t, 256> a{}, b{}, expected{};
    unsigned cases = 0;
    std::uint64_t compared_frames = 0, hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<std::int16_t>(static_cast<int>((i * 997 + 143) % 50001) - 25000);

    for (auto mode : {v::LoopMode::Continuous, v::LoopMode::UntilRelease})
    for (unsigned fade : {16u, 32u, 64u})
    for (auto step : {Q / 2, Q, 5 * Q / 4})
    for (unsigned release : {0u, 32u})
    for (unsigned profile : {0u, 1u}) {
        record.bank_generation = 1;
        record.sample_generation = cases + 1;
        record.pcm = pcm.data();
        record.descriptor = {};
        record.descriptor.id = 17;
        record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.flags = 1;
        record.descriptor.loop_start = 23;
        record.descriptor.loop_end = 523;
        V1_REQUIRE(directory.publish(17, &record));

        x::LoopPlanContext context{};
        // Synthetic identity for this immutable test fixture, NOT a bank digest.
        context.asset_sha256.fill(0xa5);
        context.session_serial = 9;
        context.bank_generation = 1;
        context.sample_generation = cases + 1;
        context.sample_id = 17;
        context.sample_frames = pcm.size();
        context.sample_rate = 44100;
        context.noteon_addresses_fixed = true;
        context.region = {11, 573, 23, 523};
        context.mode = mode == v::LoopMode::Continuous ?
            x::CursorLoopMode::Continuous : x::CursorLoopMode::UntilRelease;
        x::LoopPlanRequest proposal{};
        proposal.context = context;
        proposal.experimental_opt_in = true;
        proposal.fade = {true, static_cast<std::uint16_t>(fade)};
        proposal.release = {release != 0, static_cast<std::uint16_t>(release)};
        x::PinnedLoopPlan plan;
        V1_REQUIRE(plan.bind(directory, proposal, context) == x::LoopBindingStatus::Ok);

        v::VoiceStartParams p{};
        p.sample_id = 17;
        p.channel = 2;
        p.loop_mode = mode;
        p.sample_region = {true, 11, 573, 23, 523};
        p.phase_increment_q32 = step;
        p.envelope = {3, 11, 5, 23, 23000, 137};
        p.gain_left_q15 = 27000;
        p.gain_right_q15 = 17000;
        if (profile) p.lowpass = {true, 1 << 22, 1 << 23, 0, 0};
        auto reference_params = p;
        p.experimental_loop_opt_in = true;
        p.experimental_loop_plan = &plan;
        p.experimental_loop_context = &context;
        auto stale_context = context;
        ++stale_context.sample_generation;
        auto bad = p;
        bad.experimental_loop_context = &stale_context;
        V1_REQUIRE(!rejected.start(directory, bad));
        V1_REQUIRE(directory.stats().live_voice_refs == 1);
        V1_REQUIRE(generic.start(directory, p));
        V1_REQUIRE(realtime.start(directory, p));
        V1_REQUIRE(directory.stats().live_voice_refs == 3);

        const unsigned off = static_cast<unsigned>(((523 - fade / 2 - 11) * Q + step - 1) / step);
        x::BoundedLoopReleaseCursor transport;
        V1_REQUIRE(transport.start(pcm.data(), pcm.size(), context.region,
            context.mode, step, proposal.fade, proposal.release));
        std::size_t count = 0;
        for (; count < stream.size(); ++count) {
            if (count == off) transport.noteOff();
            if (!transport.next(stream[count])) break;
        }
        transport.reset();
        stream_record.bank_generation = 1;
        stream_record.sample_generation = cases + 1;
        stream_record.pcm = stream.data();
        stream_record.descriptor = record.descriptor;
        stream_record.descriptor.id = 18;
        stream_record.descriptor.frames = count;
        stream_record.descriptor.pcm_bytes = count * sizeof(std::int16_t);
        stream_record.descriptor.loop_start = stream_record.descriptor.loop_end = 0;
        V1_REQUIRE(offline.publish(18, &stream_record));
        reference_params.sample_id = 18;
        reference_params.sample_region = {};
        reference_params.phase_increment_q32 = Q;
        reference_params.loop_mode = v::LoopMode::None;
        V1_REQUIRE(reference.start(offline, reference_params));

        // The production Voices must retain the OLD PCM after both publication
        // and the plan are retired. This is not app Program-change orchestration.
        V1_REQUIRE(directory.unpublish(17) == &record);
        V1_REQUIRE(plan.reset());
        V1_REQUIRE(!directory.canReclaim(&record));
        constexpr unsigned blocks[] = {1, 17, 64, 3, 31, 128};
        unsigned done = 0, block_index = 0;
        while (done < stream.size()) {
            if (done == off) { generic.noteOff(); realtime.noteOff(); reference.noteOff(); }
            if (done == 67) {
                v::VoiceRuntimeControlUpdate update{};
                update.phase_increment_q32 = step;
                update.gain_left_q15 = 19000;
                update.gain_right_q15 = 23000;
                V1_REQUIRE(generic.applyRuntimeControlUpdate(update));
                V1_REQUIRE(realtime.applyRuntimeControlUpdate(update));
                update.phase_increment_q32 = Q;
                V1_REQUIRE(reference.applyRuntimeControlUpdate(update));
            }
            unsigned n = std::min<unsigned>(blocks[block_index++ % 6], stream.size() - done);
            if (done < off) n = std::min(n, off - done);
            if (done < 67) n = std::min(n, 67 - done);
            a.fill(7); b.fill(7); expected.fill(7);
            const auto ra = generic.renderAdd(a.data(), n);
            const auto rb = realtime.renderAddRealtime(b.data(), n);
            const auto rr = reference.renderAdd(expected.data(), n);
            V1_REQUIRE(ra.frames_rendered == rb.frames_rendered && ra.frames_rendered == rr.frames_rendered);
            V1_REQUIRE(ra.finished == rb.finished && ra.finished == rr.finished);
            V1_REQUIRE(generic.phaseQ32() == realtime.phaseQ32());
            V1_REQUIRE(a == b && a == expected);
            for (unsigned i = 0; i < n * 2; ++i) {
                hash ^= static_cast<std::uint32_t>(a[i]);
                hash *= UINT64_C(1099511628211);
            }
            compared_frames += ra.frames_rendered;
            done += n;
        }
        V1_REQUIRE(!generic.active() && !realtime.active() && !reference.active());
        generic.reset(); realtime.reset(); reference.reset(); rejected.reset();
        V1_REQUIRE(offline.unpublish(18) == &stream_record);
        V1_REQUIRE(directory.canReclaim(&record) && offline.canReclaim(&stream_record));
        V1_REQUIRE(directory.stats().live_voice_refs == 0 && offline.stats().live_voice_refs == 0);
        V1_REQUIRE(directory.stats().stale_release_faults == 0 && offline.stats().stale_release_faults == 0);
        ++cases;
        if (yield_between_cases) yield_between_cases();
    }
    V1_REQUIRE(cases == 72);
    std::printf("V1_TARGET_RESULT cases=%u frames=%" PRIu64 " hash=%" PRIu64 "\n", cases, compared_frames, hash);
    std::printf("V1_TARGET_LAYOUT pointer=%u voice=%u params=%u pool=%u\n",
        static_cast<unsigned>(sizeof(void*)), static_cast<unsigned>(sizeof(v::Voice)),
        static_cast<unsigned>(sizeof(v::VoiceStartParams)), static_cast<unsigned>(sizeof(v::VoicePool)));
    std::fflush(stdout);
}
#undef V1_REQUIRE
} // namespace loop_target_test
