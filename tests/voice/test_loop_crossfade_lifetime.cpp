#include "voice/experimental/PinnedLoopPlan.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>

namespace ex = gm_synth::voice::experimental;
namespace sm = gm_synth::sample;
using Status = ex::LoopBindingStatus;
constexpr auto Q = ex::LoopCrossfadeCursor::kOne;
static_assert(!std::is_copy_constructible_v<ex::PinnedLoopPlan>);
static_assert(!std::is_move_constructible_v<ex::PinnedLoopPlan>);
static_assert(!std::is_copy_assignable_v<ex::PinnedLoopPlayback>);
static_assert(!std::is_move_assignable_v<ex::PinnedLoopPlayback>);

struct Fixture {
    sm::PublishedSampleDirectory directory;
    std::array<std::int16_t, 603> pcm{};
    sm::ResidentRecord record{};
    explicit Fixture(sm::SampleId id = 17) {
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::int16_t>(static_cast<int>((i * 997u + 143u) % 60001u) - 30000);
        init(record, pcm.data(), id, 1);
        assert(directory.publish(id, &record));
    }
    void init(sm::ResidentRecord& r, const std::int16_t* data, sm::SampleId id,
              std::uint32_t generation) {
        r.bank_generation = directory.bankGeneration();
        r.sample_generation = generation;
        r.pcm = data;
        r.descriptor.id = id;
        r.descriptor.frames = pcm.size();
        r.descriptor.pcm_bytes = pcm.size() * sizeof(pcm[0]);
        r.descriptor.sample_rate = 44100;
        r.descriptor.loop_start = 19;
        r.descriptor.loop_end = 531;
    }
    ex::LoopPlanContext context() const {
        ex::LoopPlanContext c;
        c.asset_sha256.fill(0xa5);
        c.session_serial = 9;
        c.bank_generation = record.bank_generation;
        c.sample_generation = record.sample_generation;
        c.sample_id = record.descriptor.id;
        c.sample_frames = record.descriptor.frames;
        c.sample_rate = record.descriptor.sample_rate;
        c.region = {11, 573, 23, 523};
        c.mode = ex::CursorLoopMode::UntilRelease;
        c.noteon_addresses_fixed = true;
        return c;
    }
    ex::LoopPlanRequest request() const {
        ex::LoopPlanRequest r;
        r.context = context();
        r.experimental_opt_in = true;
        r.fade = {true, 32};
        r.release = {true, 32};
        return r;
    }
    void clean() const { assert(directory.stats().live_voice_refs == 0); assert(directory.stats().stale_release_faults == 0); }
};

void identities() {
    Fixture f;
    auto p = f.request();
    ex::PinnedLoopPlan plan;
    auto denied = p; denied.experimental_opt_in = false;
    assert(plan.bind(f.directory, denied, p.context) == Status::OptInRequired);
    for (unsigned field = 0; field < 16; ++field) {
        auto other = p.context;
        switch (field) {
        case 0: other.asset_sha256[4] ^= 1; break;
        case 1: ++other.session_serial; break;
        case 2: ++other.bank_generation; break;
        case 3: ++other.sample_generation; break;
        case 4: ++other.sample_id; break;
        case 5: ++other.sample_frames; break;
        case 6: ++other.sample_rate; break;
        case 7: ++other.region.start; break;
        case 8: --other.region.end; break;
        case 9: ++other.region.loop_start; break;
        case 10: --other.region.loop_end; break;
        case 11: other.mode = ex::CursorLoopMode::Continuous; break;
        case 12: ++other.policy_revision; break;
        case 13: ++other.coordinate_revision; break;
        case 14: other.sample_type = 2; break;
        case 15: other.noteon_addresses_fixed = false; break;
        }
        assert(plan.bind(f.directory, p, other) != Status::Ok);
        assert(!plan.valid()); f.clean();
    }
    for (unsigned field = 0; field < 5; ++field) {
        auto invalid = p;
        if (field == 0) invalid.context.asset_sha256.fill(0);
        if (field == 1) invalid.context.session_serial = 0;
        if (field == 2) invalid.context.bank_generation = 0;
        if (field == 3) invalid.context.sample_generation = 0;
        if (field == 4) invalid.context.sample_rate = 0;
        assert(plan.bind(f.directory, invalid, invalid.context) == Status::InvalidContext);
    }
    assert(plan.bind(f.directory, p, p.context) == Status::Ok);
    assert(f.directory.stats().live_voice_refs == 1);
    auto copied = plan.context(); ++copied.region.loop_start;
    assert(ex::sameContext(plan.context(), p.context));
    assert(plan.bind(f.directory, p, copied) == Status::Busy);
    ex::PinnedLoopPlayback reader;
    assert(reader.start(f.directory, plan, copied) == Status::ContextMismatch);
    sm::PublishedSampleDirectory wrong;
    assert(reader.start(wrong, plan, p.context) == Status::WrongDirectory);
    assert(reader.start(f.directory, plan, p.context, 0) == Status::InvalidConfiguration);
    assert(f.directory.stats().live_voice_refs == 1);
    assert(plan.reset()); assert(plan.reset()); f.clean();
}

void malformed() {
    Fixture f;
    for (unsigned field = 0; field < 12; ++field) {
        auto p = f.request();
        switch (field) {
        case 0: p.fade.frames = 17; break;
        case 1: p.fade = {false, 32}; break;
        case 2: p.release = {false, 32}; break;
        case 3: p.release.output_frames = 17; break;
        case 4: p.context.region.end = 9999; break;
        case 5: p.context.region.start = p.context.region.end; break;
        case 6: p.context.region.loop_end = p.context.region.loop_start; break;
        case 7: p.context.region.loop_start = 0; break;
        case 8: p.context.region.loop_end = 900; break;
        case 9: p.context.mode = ex::CursorLoopMode::None; break;
        case 10: p.context.region.loop_end = p.context.region.loop_start + 64; break;
        case 11: p.context.mode = static_cast<ex::CursorLoopMode>(2); break;
        }
        ex::PinnedLoopPlan plan;
        assert(plan.bind(f.directory, p, p.context) == Status::InvalidConfiguration);
        f.clean();
    }
    const auto p = f.request();
    for (unsigned field = 0; field < 4; ++field) {
        auto mismatch = p;
        if (field == 0) ++mismatch.context.sample_generation;
        if (field == 1) ++mismatch.context.bank_generation;
        if (field == 2) ++mismatch.context.sample_frames;
        if (field == 3) ++mismatch.context.sample_rate;
        ex::PinnedLoopPlan plan;
        assert(plan.bind(f.directory, mismatch, mismatch.context) == Status::RecordMismatch);
        f.clean();
    }
    assert(f.directory.unpublish(p.context.sample_id) == &f.record);
    f.record.descriptor.pcm_bytes = 2;
    assert(f.directory.publish(p.context.sample_id, &f.record));
    ex::PinnedLoopPlan plan;
    assert(plan.bind(f.directory, p, p.context) == Status::RecordMismatch);
    f.clean();
    assert(f.directory.unpublish(p.context.sample_id) == &f.record);
    assert(plan.bind(f.directory, p, p.context) == Status::SampleUnavailable);
}

void retirement_and_replacement() {
    Fixture f;
    const auto p = f.request();
    ex::PinnedLoopPlan plan;
    ex::PinnedLoopPlayback a, b, fresh;
    assert(plan.bind(f.directory, p, p.context) == Status::Ok);
    assert(a.start(f.directory, plan, p.context) == Status::Ok);
    assert(b.start(f.directory, plan, p.context) == Status::Ok);
    assert(f.record.voice_refs == 3);
    assert(f.directory.unpublish(p.context.sample_id) == &f.record);
    assert(!f.directory.canReclaim(&f.record));
    assert(fresh.start(f.directory, plan, p.context) == Status::SampleUnavailable);
    std::array<std::int16_t, 603> replacement_pcm{};
    replacement_pcm.fill(9999);
    sm::ResidentRecord replacement;
    f.init(replacement, replacement_pcm.data(), p.context.sample_id, 2);
    assert(f.directory.publish(p.context.sample_id, &replacement));
    assert(fresh.start(f.directory, plan, p.context) == Status::StalePublication);
    assert(replacement.voice_refs == 0 && f.record.voice_refs == 3);
    auto p2 = p; p2.context.sample_generation = 2;
    ex::PinnedLoopPlan plan2;
    assert(plan2.bind(f.directory, p, p.context) == Status::RecordMismatch);
    assert(plan2.bind(f.directory, p2, p2.context) == Status::Ok);
    assert(fresh.start(f.directory, plan2, p2.context) == Status::Ok);
    assert(plan.reset()); // readers own independent pins and copied settings
    assert(f.record.voice_refs == 2);
    std::int16_t x = 0, y = 0, z = 0;
    assert(a.next(x) && b.next(y) && fresh.next(z));
    assert(x == f.pcm[p.context.region.start] && x == y && z == 9999);
    assert(!f.directory.setBankGeneration(2));
    assert(a.reset()); assert(!f.directory.canReclaim(&f.record));
    assert(b.reset()); assert(f.directory.canReclaim(&f.record));
    assert(plan2.reset()); assert(fresh.reset());
    assert(f.directory.unpublish(p.context.sample_id) == &replacement);
    assert(f.directory.canReclaim(&replacement));
    assert(f.directory.setBankGeneration(2));
    // An invalidated plan never reactivates when an ID becomes available again.
    assert(a.start(f.directory, plan, p.context) == Status::MissingPlan);
    f.clean();
}

void independent_geometries_and_destruction() {
    Fixture f(0xf1234567u);
    auto p = f.request(), q = p; q.context.region.loop_start += 17;
    ex::PinnedLoopPlan plan, other;
    assert(plan.bind(f.directory, p, p.context) == Status::Ok);
    assert(other.bind(f.directory, q, q.context) == Status::Ok);
    {
        ex::PinnedLoopPlayback a, b;
        assert(a.start(f.directory, plan, q.context) == Status::ContextMismatch);
        assert(a.start(f.directory, plan, p.context) == Status::Ok);
        assert(b.start(f.directory, other, q.context) == Status::Ok);
        assert(f.record.voice_refs == 4);
        assert(a.start(f.directory, other, q.context) == Status::Busy);
        const auto phase = a.phaseQ32();
        std::int16_t untouched = 123;
        assert(a.render(nullptr, 8) == 0 && a.render(&untouched, 0) == 0);
        assert(untouched == 123 && a.phaseQ32() == phase);
    }
    assert(f.record.voice_refs == 2);
    assert(plan.reset()); assert(other.reset()); f.clean();
    { ex::PinnedLoopPlan scoped; assert(scoped.bind(f.directory, p, p.context) == Status::Ok); }
    f.clean();
}

void pcm_parity() {
    unsigned cases = 0;
    for (auto mode : {ex::CursorLoopMode::None, ex::CursorLoopMode::Continuous, ex::CursorLoopMode::UntilRelease})
    for (unsigned fade : {0u, 16u, 32u, 64u}) {
        if (mode == ex::CursorLoopMode::None && fade) continue;
        for (auto step : {Q / 2, Q, 5 * Q / 4, 3 * Q + 11})
        for (unsigned off : {0u, 500u, 1500u})
        for (unsigned block : {1u, 17u, 64u}) {
            Fixture f;
            auto p = f.request(); p.context.mode = mode;
            p.fade = {fade != 0, static_cast<std::uint16_t>(fade)};
            p.release = {fade != 0, static_cast<std::uint16_t>(fade ? 32 : 0)};
            ex::PinnedLoopPlan plan;
            ex::PinnedLoopPlayback actual;
            ex::BoundedLoopReleaseCursor expected;
            assert(plan.bind(f.directory, p, p.context) == Status::Ok);
            assert(actual.start(f.directory, plan, p.context, step) == Status::Ok);
            assert(expected.start(f.pcm.data(), f.pcm.size(), p.context.region, mode, step, p.fade, p.release));
            assert(plan.reset()); // no dependence on plan memory while rendering
            std::array<std::int16_t,64> a{}, b{};
            for (unsigned frame = 0; frame < 1700;) {
                if (frame == off) { actual.noteOff(); actual.noteOff(); expected.noteOff(); }
                unsigned count = block;
                if (frame < off && frame + count > off) count = off - frame;
                if (frame + count > 1700) count = 1700 - frame;
                a.fill(123); b.fill(123);
                const auto na = actual.render(a.data(), count), nb = expected.render(b.data(), count);
                assert(na == nb && a == b);
                assert(actual.active() == expected.active());
                if (actual.active()) assert(actual.phaseQ32() == expected.phaseQ32());
                else assert(!actual.holdsSample());
                frame += count;
            }
            assert(actual.reset()); f.clean(); ++cases;
        }
    }
    assert(cases == 324);
    std::cout << "Pinned PCM/region/mode/block parity: " << cases << " cases PASS\n";
}

void release_shadow_and_reclamation() {
    Fixture f;
    auto p = f.request();
    ex::PinnedLoopPlan plan;
    ex::PinnedLoopPlayback reader;
    assert(plan.bind(f.directory, p, p.context) == Status::Ok);
    assert(reader.start(f.directory, plan, p.context) == Status::Ok);
    std::int16_t out;
    while ((reader.phaseQ32() >> 32) < 511) assert(reader.next(out));
    reader.noteOff();
    assert(reader.transitionActive());
    assert(f.directory.unpublish(p.context.sample_id) == &f.record);
    assert(plan.reset());
    assert(!f.directory.canReclaim(&f.record)); // shadow shares the ONE playback pin
    assert(f.record.voice_refs == 1);
    unsigned produced = 0;
    while (reader.next(out)) ++produced;
    assert(produced == 62);
    assert(!reader.holdsSample() && f.directory.canReclaim(&f.record));
    f.clean();
    // Physically delete PCM/record only after the protocol allows it, then call
    // inactive operations under ASan to catch hidden retained-pointer reads.
    sm::PublishedSampleDirectory directory;
    auto pcm = std::make_unique<std::array<std::int16_t,603>>();
    auto record = std::make_unique<sm::ResidentRecord>();
    f.init(*record, pcm->data(), 17, 1);
    assert(directory.publish(17, record.get()));
    assert(plan.bind(directory, p, p.context) == Status::Ok);
    assert(reader.start(directory, plan, p.context) == Status::Ok);
    assert(directory.unpublish(17) == record.get());
    assert(plan.reset()); assert(!directory.canReclaim(record.get()));
    assert(reader.reset()); assert(directory.canReclaim(record.get()));
    record.reset(); pcm.reset();
    reader.noteOff(); assert(!reader.next(out)); assert(!reader.setStep(Q));
    assert(reader.reset() && plan.reset());
}

void concurrent_directory_retirement() {
    Fixture f;
    auto p = f.request();
    ex::PinnedLoopPlan plan;
    assert(plan.bind(f.directory, p, p.context) == Status::Ok);
    std::array<std::int16_t,603> second_pcm{};
    sm::ResidentRecord second;
    f.init(second, second_pcm.data(), p.context.sample_id, 2);
    std::atomic<unsigned> ready{0}, attempts{0};
    std::atomic<bool> go{false};
    auto audio = [&] {
        ++ready;
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 2000; ++i) {
            ex::PinnedLoopPlayback r;
            const auto s = r.start(f.directory, plan, p.context);
            assert(s == Status::Ok || s == Status::SampleUnavailable || s == Status::StalePublication);
            if (s == Status::Ok) {
                std::int16_t out;
                assert(r.next(out) && out == f.pcm[p.context.region.start]);
                assert(!f.directory.canReclaim(&f.record));
            }
            ++attempts;
        }
    };
    std::thread a(audio), b(audio);
    while (ready.load() != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 2000; ++i) {
        (void)f.directory.unpublish(p.context.sample_id);
        assert(f.directory.publish(p.context.sample_id, (i & 1u) ? &f.record : &second));
    }
    a.join(); b.join();
    assert(attempts == 4000);
    (void)f.directory.unpublish(p.context.sample_id);
    assert(plan.reset());
    assert(f.directory.canReclaim(&f.record) && f.directory.canReclaim(&second));
    f.clean();
    std::cout << "Concurrent directory retire/replace: 4000 attempts PASS (stable plan)\n";
}

int main() {
    identities(); malformed(); retirement_and_replacement();
    independent_geometries_and_destruction(); pcm_parity();
    release_shadow_and_reclamation(); concurrent_directory_retirement();
    std::cout << "Pinned loop lifetime: 7 suites PASS; production integration disabled\n";
    std::cout << "Host-only sizeof: context=" << sizeof(ex::LoopPlanContext)
              << " plan=" << sizeof(ex::PinnedLoopPlan) << " playback=" << sizeof(ex::PinnedLoopPlayback) << '\n';
}
