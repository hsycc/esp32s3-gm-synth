#include "../../firmware/sample/ProgramSnapshot.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"
#include "../../firmware/sample/SampleResidencyManager.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <thread>
#include <vector>

using gm_synth::sample::ProgramSnapshot;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::PublishedVoiceRef;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleRef;
using gm_synth::sample::SampleResidencyManager;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

SampleDescriptor descriptor(std::uint32_t id, std::uint32_t bytes) {
    SampleDescriptor d{};
    d.id = id;
    d.pcm_bytes = bytes;
    d.frames = bytes / 2;
    d.loop_start = bytes >= 8 ? 1 : 0;
    d.loop_end = bytes >= 8 ? (bytes / 2) - 1 : bytes / 2;
    d.sample_rate = 44100;
    d.root_key = 60;
    d.pitch_correction = 0;
    d.flags = 1;
    return d;
}

void initRecord(
    ResidentRecord& record,
    std::uint32_t bank_generation,
    std::uint32_t sample_generation,
    std::uint32_t sample_id,
    const std::int16_t* pcm,
    std::uint32_t pcm_bytes) {
    record.bank_generation = bank_generation;
    record.sample_generation = sample_generation;
    record.pcm = pcm;
    record.descriptor = descriptor(sample_id, pcm_bytes);
    record.voice_refs.store(0, std::memory_order_release);
}

ProgramSnapshot snapshot(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token,
    std::initializer_list<std::uint32_t> dependencies) {
    ProgramSnapshot out{};
    out.channel = channel;
    out.bank = 0;
    out.program = program;
    out.token = token;
    out.bank_generation = 1;
    out.dependency_count = static_cast<std::uint16_t>(dependencies.size());
    std::size_t index = 0;
    for (const std::uint32_t id : dependencies) {
        out.sample_ids[index++] = id;
    }
    return out;
}

bool pinSnapshot(SampleResidencyManager& manager, const ProgramSnapshot& snap) {
    for (std::size_t i = 0; i < snap.dependency_count; ++i) {
        if (!manager.pin(snap.sample_ids[i])) {
            while (i > 0) {
                --i;
                (void)manager.unpin(snap.sample_ids[i]);
            }
            return false;
        }
    }
    return true;
}

bool unpinSnapshot(SampleResidencyManager& manager, const ProgramSnapshot& snap) {
    bool ok = true;
    for (std::size_t i = 0; i < snap.dependency_count; ++i) {
        ok = manager.unpin(snap.sample_ids[i]) && ok;
    }
    return ok;
}

void test_publish_unpublish_and_deferred_reclaim() {
    PublishedSampleDirectory directory;
    alignas(4) std::array<std::int16_t, 16> pcm{};
    ResidentRecord record{};
    initRecord(record, 1, 10, 3, pcm.data(), static_cast<std::uint32_t>(pcm.size() * 2));

    CHECK(directory.publish(3, &record));
    CHECK(directory.published(3) == &record);

    const PublishedVoiceRef voice = directory.acquire(3);
    CHECK(voice.valid());
    CHECK(voice.record == &record);
    CHECK(record.voice_refs.load(std::memory_order_acquire) == 1);
    CHECK(directory.stats().live_voice_refs == 1);

    CHECK(directory.unpublish(3) == &record);
    CHECK(!directory.acquire(3).valid());
    CHECK(!directory.canReclaim(&record));
    CHECK(!directory.setBankGeneration(2));

    CHECK(directory.release(voice));
    CHECK(record.voice_refs.load(std::memory_order_acquire) == 0);
    CHECK(directory.canReclaim(&record));
    CHECK(directory.setBankGeneration(2));
}

void test_publish_conflict_requires_explicit_unpublish() {
    PublishedSampleDirectory directory;
    alignas(4) std::array<std::int16_t, 8> pcm_a{};
    alignas(4) std::array<std::int16_t, 8> pcm_b{};
    ResidentRecord a{};
    ResidentRecord b{};
    initRecord(a, 1, 1, 5, pcm_a.data(), static_cast<std::uint32_t>(pcm_a.size() * 2));
    initRecord(b, 1, 2, 5, pcm_b.data(), static_cast<std::uint32_t>(pcm_b.size() * 2));

    CHECK(directory.publish(5, &a));
    CHECK(!directory.publish(5, &b));
    CHECK(directory.stats().publish_conflicts == 1);
    CHECK(directory.unpublish(5) == &a);
    CHECK(directory.canReclaim(&a));
    CHECK(directory.publish(5, &b));
    CHECK(directory.unpublish(5) == &b);
    CHECK(directory.canReclaim(&b));
}

void test_concurrent_publish_unpublish_acquire_release_stress() {
    PublishedSampleDirectory directory;
    CHECK(directory.setBankGeneration(7));

    alignas(4) std::array<std::int16_t, 32> pcm_a{};
    alignas(4) std::array<std::int16_t, 32> pcm_b{};
    ResidentRecord a{};
    ResidentRecord b{};
    initRecord(a, 7, 101, 7, pcm_a.data(), static_cast<std::uint32_t>(pcm_a.size() * 2));
    initRecord(b, 7, 102, 7, pcm_b.data(), static_cast<std::uint32_t>(pcm_b.size() * 2));
    CHECK(directory.publish(7, &a));

    std::atomic<bool> stop{false};
    std::atomic<int> thread_failures{0};
    std::vector<std::thread> readers;
    readers.reserve(4);

    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                const PublishedVoiceRef ref = directory.acquire(7);
                if (!ref.valid()) {
                    std::this_thread::yield();
                    continue;
                }
                if (ref.sample_id != 7 || ref.bank_generation != 7 ||
                    (ref.sample_generation != 101 && ref.sample_generation != 102) ||
                    ref.record->pcm == nullptr) {
                    thread_failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (!directory.release(ref)) {
                    thread_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    const ResidentRecord* current = &a;
    for (int iteration = 0; iteration < 250; ++iteration) {
        const ResidentRecord* retiring = directory.unpublish(7);
        CHECK(retiring == current);
        while (!directory.canReclaim(retiring)) {
            std::this_thread::yield();
        }
        CHECK(retiring->voice_refs.load(std::memory_order_acquire) == 0);

        current = (current == &a) ? &b : &a;
        CHECK(directory.publish(7, current));
    }

    const ResidentRecord* retiring = directory.unpublish(7);
    CHECK(retiring == current);
    while (!directory.canReclaim(retiring)) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);

    for (std::thread& reader : readers) {
        reader.join();
    }

    CHECK(thread_failures.load(std::memory_order_acquire) == 0);
    CHECK(directory.stats().live_voice_refs == 0);
    CHECK(directory.stats().active_acquirers == 0);
    CHECK(directory.canReclaim(retiring));
}

void test_program_snapshot_block_boundary_and_ack() {
    ProgramSnapshotExchange exchange;
    const ProgramSnapshot a = snapshot(0, 8, 1, {1, 2});
    const ProgramSnapshot b = snapshot(0, 16, 2, {2, 3});
    const ProgramSnapshot c = snapshot(0, 32, 3, {4});

    CHECK(exchange.setInitialActive(&a));
    CHECK(exchange.active(0) == &a);
    CHECK(exchange.publishReady(&b));
    CHECK(exchange.ready(0) == &b);
    CHECK(exchange.active(0) == &a);
    CHECK(exchange.transitionInFlight(0));

    // V1 bounds rapid Program Change: one READY/commit/ack transition at once.
    CHECK(!exchange.publishReady(&c));

    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(exchange.active(0) == &b);
    CHECK(exchange.ready(0) == nullptr);
    CHECK(exchange.committedToken(0) == b.token);
    CHECK(exchange.transitionInFlight(0));
    CHECK(!exchange.publishReady(&c));

    CHECK(!exchange.acknowledgeCommit(0, 999).valid());
    const auto ack_b = exchange.acknowledgeCommit(0, b.token);
    CHECK(ack_b.valid());
    CHECK(ack_b.retired == &a);
    CHECK(!exchange.transitionInFlight(0));

    CHECK(exchange.publishReady(&c));
    CHECK(exchange.commitAtBlockBoundary(0));
    const auto ack_c = exchange.acknowledgeCommit(0, c.token);
    CHECK(ack_c.valid());
    CHECK(ack_c.retired == &b);
    CHECK(exchange.active(0) == &c);

    const auto stats = exchange.stats();
    CHECK(stats.ready_publishes == 2);
    CHECK(stats.ready_rejections == 2);
    CHECK(stats.commits == 2);
    CHECK(stats.commit_acks == 2);
}

void test_program_pin_retire_keeps_release_tail_and_shared_sample_alive() {
    alignas(4) std::array<std::uint8_t, 128> arena{};
    std::array<std::uint8_t, 16> pcm1{};
    std::array<std::uint8_t, 16> pcm2{};
    std::array<std::uint8_t, 16> pcm3{};
    SampleResidencyManager manager;
    ProgramSnapshotExchange exchange;

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(1, 16), pcm1.data(), pcm1.size()));
    CHECK(manager.load(descriptor(2, 16), pcm2.data(), pcm2.size()));
    CHECK(manager.load(descriptor(3, 16), pcm3.data(), pcm3.size()));

    const ProgramSnapshot old_program = snapshot(0, 8, 10, {1, 2});
    const ProgramSnapshot new_program = snapshot(0, 16, 11, {2, 3});

    CHECK(pinSnapshot(manager, old_program));
    CHECK(exchange.setInitialActive(&old_program));

    // Voice 1 becomes a release tail from the old program.
    const SampleRef release_tail = manager.acquire(1);
    CHECK(release_tail.valid());

    // Prepare owns pins before READY publication.
    CHECK(pinSnapshot(manager, new_program));
    CHECK(exchange.publishReady(&new_program));
    CHECK(exchange.commitAtBlockBoundary(0));
    const auto ack = exchange.acknowledgeCommit(0, new_program.token);
    CHECK(ack.retired == &old_program);

    // Control retires only the old snapshot's pins after the commit ack.
    CHECK(unpinSnapshot(manager, *ack.retired));

    // Sample 1 lost its program pin but is protected by the old release-tail
    // Voice Ref. Sample 2 is still pinned by the new program because residency
    // is shared across snapshots/channels.
    CHECK(!manager.evict(1));
    CHECK(!manager.evict(2));

    CHECK(manager.release(release_tail));
    CHECK(manager.evict(1));

    CHECK(unpinSnapshot(manager, new_program));
    CHECK(manager.evict(2));
    CHECK(manager.evict(3));
}

}  // namespace

int main() {
    test_publish_unpublish_and_deferred_reclaim();
    test_publish_conflict_requires_explicit_unpublish();
    test_concurrent_publish_unpublish_acquire_release_stress();
    test_program_snapshot_block_boundary_and_ack();
    test_program_pin_retire_keeps_release_tail_and_shared_sample_alive();

    if (failures != 0) {
        std::fprintf(stderr, "%d sample publication checks failed\n", failures);
        return 1;
    }
    std::printf("sample publication host checks passed\n");
    return 0;
}
