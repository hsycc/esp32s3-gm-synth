#include "../../firmware/sample/SampleResidencyManager.h"

#include <array>
#include <cstdint>
#include <cstdio>

using gm_synth::sample::ResidencyError;
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

template <std::size_t N>
std::array<std::uint8_t, N> payload(std::uint8_t seed) {
    std::array<std::uint8_t, N> bytes{};
    for (std::size_t i = 0; i < N; ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i);
    }
    return bytes;
}

void test_basic_pin_and_voice_lifetime() {
    alignas(4) std::array<std::uint8_t, 128> arena{};
    const auto pcm = payload<16>(3);
    SampleResidencyManager manager;

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(10, pcm.size()), pcm.data(), pcm.size()));
    CHECK(manager.pin(10));

    SampleRef voice = manager.acquire(10);
    CHECK(voice.valid());
    CHECK(voice.descriptor.id == 10);
    CHECK(manager.stats().used_bytes == 16);
    CHECK(manager.stats().pinned_sample_count == 1);
    CHECK(manager.stats().referenced_sample_count == 1);

    // Program Change can retire the program pin, but the release-tail voice
    // still owns the PCM and must block eviction.
    CHECK(manager.unpin(10));
    CHECK(!manager.evict(10));
    CHECK(manager.lastError() == ResidencyError::Busy);
    CHECK(manager.isResident(10));

    CHECK(manager.release(voice));
    CHECK(manager.evict(10));
    CHECK(!manager.isResident(10));
    CHECK(manager.stats().used_bytes == 0);
    CHECK(manager.stats().largest_free_span_bytes == arena.size());
}

void test_duplicate_load_reuses_allocation_and_rejects_conflict() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm = payload<16>(9);
    SampleResidencyManager manager;

    CHECK(manager.begin(arena.data(), arena.size()));
    const SampleDescriptor original = descriptor(22, pcm.size());
    CHECK(manager.load(original, pcm.data(), pcm.size()));
    const auto before = manager.stats();

    CHECK(manager.load(original, pcm.data(), pcm.size()));
    const auto after = manager.stats();
    CHECK(after.used_bytes == before.used_bytes);
    CHECK(after.resident_sample_count == 1);

    SampleDescriptor conflicting = original;
    conflicting.loop_start += 1;
    CHECK(!manager.load(conflicting, pcm.data(), pcm.size()));
    CHECK(manager.lastError() == ResidencyError::DescriptorMismatch);
    CHECK(manager.stats().descriptor_conflicts == 1);
    CHECK(manager.stats().resident_sample_count == 1);
}

void test_fragmentation_is_distinct_from_total_free_space() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm16 = payload<16>(1);
    const auto pcm24 = payload<24>(2);
    SampleResidencyManager manager;

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(1, 16), pcm16.data(), pcm16.size()));
    CHECK(manager.load(descriptor(2, 16), pcm16.data(), pcm16.size()));
    CHECK(manager.load(descriptor(3, 16), pcm16.data(), pcm16.size()));
    CHECK(manager.stats().used_bytes == 48);

    CHECK(manager.evict(2));
    const auto fragmented = manager.stats();
    CHECK(fragmented.used_bytes == 32);
    CHECK(fragmented.largest_free_span_bytes == 16);
    CHECK(fragmented.free_span_count == 2);

    // 32 bytes are free in total, but only as two disjoint 16-byte spans.
    CHECK(!manager.load(descriptor(4, 24), pcm24.data(), pcm24.size()));
    CHECK(manager.lastError() == ResidencyError::NoContiguousSpace);
    CHECK(manager.stats().allocation_failures == 1);

    // Freeing the adjacent third sample coalesces the middle and tail spans.
    CHECK(manager.evict(3));
    CHECK(manager.stats().largest_free_span_bytes == 48);
    CHECK(manager.load(descriptor(4, 24), pcm24.data(), pcm24.size()));
}

void test_stale_and_double_release_are_rejected() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm = payload<16>(4);
    SampleResidencyManager manager;

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(7, 16), pcm.data(), pcm.size()));

    SampleRef first = manager.acquire(7);
    CHECK(first.valid());
    CHECK(manager.release(first));
    CHECK(!manager.release(first));
    CHECK(manager.lastError() == ResidencyError::StaleReference);

    SampleRef second = manager.acquire(7);
    CHECK(second.valid());
    SampleRef stale_copy = second;
    CHECK(manager.release(second));
    CHECK(manager.evict(7));
    CHECK(manager.load(descriptor(7, 16), pcm.data(), pcm.size()));
    CHECK(!manager.release(stale_copy));
    CHECK(manager.lastError() == ResidencyError::StaleReference);
    CHECK(manager.stats().stale_reference_faults == 2);
}

void test_reset_refuses_to_invalidate_live_owners() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm = payload<16>(5);
    SampleResidencyManager manager;

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(9, 16), pcm.data(), pcm.size()));
    CHECK(manager.pin(9));
    CHECK(!manager.reset());
    CHECK(manager.lastError() == ResidencyError::Busy);

    CHECK(manager.unpin(9));
    SampleRef voice = manager.acquire(9);
    CHECK(voice.valid());
    CHECK(!manager.reset());
    CHECK(manager.lastError() == ResidencyError::Busy);

    CHECK(manager.release(voice));
    const auto high_water_before = manager.stats().high_water_bytes;
    CHECK(manager.reset());
    CHECK(manager.stats().resident_sample_count == 0);
    CHECK(manager.stats().used_bytes == 0);
    CHECK(manager.stats().largest_free_span_bytes == arena.size());
    CHECK(manager.stats().high_water_bytes == high_water_before);
}

void test_voice_lease_capacity_is_bounded() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm = payload<16>(6);
    SampleResidencyManager manager;
    std::array<SampleRef, SampleResidencyManager::kMaxVoiceLeases> refs{};

    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(11, 16), pcm.data(), pcm.size()));

    for (std::size_t i = 0; i < refs.size(); ++i) {
        refs[i] = manager.acquire(11);
        CHECK(refs[i].valid());
    }
    CHECK(manager.stats().active_lease_count == refs.size());

    const SampleRef overflow = manager.acquire(11);
    CHECK(!overflow.valid());
    CHECK(manager.lastError() == ResidencyError::LeaseExhausted);

    for (const SampleRef& ref : refs) {
        CHECK(manager.release(ref));
    }
    CHECK(manager.stats().active_lease_count == 0);
}

void test_high_water_and_alignment() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    const auto pcm6 = payload<6>(7);
    const auto pcm10 = payload<10>(8);
    SampleResidencyManager manager;

    CHECK(SampleResidencyManager::alignedBytes(6) == 8);
    CHECK(SampleResidencyManager::alignedBytes(10) == 12);
    CHECK(manager.begin(arena.data(), arena.size()));
    CHECK(manager.load(descriptor(31, 6), pcm6.data(), pcm6.size()));
    CHECK(manager.load(descriptor(32, 10), pcm10.data(), pcm10.size()));
    CHECK(manager.stats().used_bytes == 20);
    CHECK(manager.stats().high_water_bytes == 20);
    CHECK(manager.evict(31));
    CHECK(manager.stats().used_bytes == 12);
    CHECK(manager.stats().high_water_bytes == 20);
}

}  // namespace

int main() {
    test_basic_pin_and_voice_lifetime();
    test_duplicate_load_reuses_allocation_and_rejects_conflict();
    test_fragmentation_is_distinct_from_total_free_space();
    test_stale_and_double_release_are_rejected();
    test_reset_refuses_to_invalidate_live_owners();
    test_voice_lease_capacity_is_bounded();
    test_high_water_and_alignment();

    if (failures != 0) {
        std::fprintf(stderr, "%d sample residency checks failed\n", failures);
        return 1;
    }
    std::printf("sample residency host checks passed\n");
    return 0;
}
