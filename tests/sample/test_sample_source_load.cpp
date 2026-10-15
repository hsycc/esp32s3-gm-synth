#include "../../firmware/sample/SampleResidencyManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using gm_synth::sample::ResidencyError;
using gm_synth::sample::SampleDescriptor;
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
    d.loop_start = 1;
    d.loop_end = bytes / 2 - 1;
    d.sample_rate = 44100;
    d.root_key = 60;
    d.flags = 1;
    return d;
}

struct FillContext {
    std::vector<std::uint8_t> source;
    std::size_t chunk_bytes = 7;
    std::size_t fail_after_bytes = 0;
    int calls = 0;
    std::uint8_t* destination = nullptr;
};

bool chunkedFill(void* opaque, std::uint8_t* destination, std::size_t pcm_bytes) {
    auto& context = *static_cast<FillContext*>(opaque);
    ++context.calls;
    context.destination = destination;
    if (context.source.size() != pcm_bytes || context.chunk_bytes == 0) {
        return false;
    }

    std::size_t copied = 0;
    while (copied < pcm_bytes) {
        const std::size_t remaining = pcm_bytes - copied;
        const std::size_t chunk = remaining < context.chunk_bytes ? remaining : context.chunk_bytes;
        std::memcpy(destination + copied, context.source.data() + copied, chunk);
        copied += chunk;
        if (context.fail_after_bytes != 0 && copied >= context.fail_after_bytes) {
            return false;
        }
    }
    return true;
}

FillContext patterned(std::size_t bytes, std::uint8_t seed) {
    FillContext context{};
    context.source.resize(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        context.source[i] = static_cast<std::uint8_t>(seed + i);
    }
    return context;
}

void test_successful_direct_fill_commits_only_after_callback() {
    alignas(4) std::array<std::uint32_t, 64> arena_words{};
    SampleResidencyManager manager;
    CHECK(manager.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    FillContext context = patterned(30, 0x20);
    context.chunk_bytes = 5;
    const SampleDescriptor desc = descriptor(9, 30);

    CHECK(manager.loadFromSource(desc, chunkedFill, &context));
    CHECK(context.calls == 1);
    CHECK(manager.isResident(9));

    const auto view = manager.residentView(9);
    CHECK(view.valid());
    CHECK(view.pcm == reinterpret_cast<const std::int16_t*>(context.destination));
    CHECK(view.descriptor == desc);
    CHECK(std::memcmp(view.pcm, context.source.data(), context.source.size()) == 0);

    const auto stats = manager.stats();
    CHECK(stats.used_bytes == 32);
    CHECK(stats.high_water_bytes == 32);
    CHECK(stats.resident_sample_count == 1);
}

void test_existing_sample_reuses_without_reading_source_again() {
    alignas(4) std::array<std::uint32_t, 32> arena_words{};
    SampleResidencyManager manager;
    CHECK(manager.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    FillContext first = patterned(16, 0x10);
    const SampleDescriptor desc = descriptor(2, 16);
    CHECK(manager.loadFromSource(desc, chunkedFill, &first));
    const auto original = manager.residentView(2);
    CHECK(original.valid());

    FillContext second = patterned(16, 0x80);
    CHECK(manager.loadFromSource(desc, chunkedFill, &second));
    CHECK(second.calls == 0);
    const auto reused = manager.residentView(2);
    CHECK(reused.pcm == original.pcm);
    CHECK(reused.generation == original.generation);
    CHECK(std::memcmp(reused.pcm, first.source.data(), first.source.size()) == 0);
}

void test_failed_partial_fill_rolls_back_span_and_visibility() {
    alignas(4) std::array<std::uint32_t, 32> arena_words{};
    SampleResidencyManager manager;
    CHECK(manager.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    FillContext failing = patterned(18, 0x30);
    failing.chunk_bytes = 6;
    failing.fail_after_bytes = 6;
    const SampleDescriptor desc = descriptor(4, 18);

    CHECK(!manager.loadFromSource(desc, chunkedFill, &failing));
    CHECK(manager.lastError() == ResidencyError::SourceFillFailed);
    CHECK(!manager.isResident(4));
    CHECK(!manager.residentView(4).valid());

    const auto after_failure = manager.stats();
    CHECK(after_failure.used_bytes == 0);
    CHECK(after_failure.resident_sample_count == 0);
    CHECK(after_failure.free_span_count == 1);
    CHECK(after_failure.largest_free_span_bytes == arena_words.size() * sizeof(std::uint32_t));

    FillContext retry = patterned(18, 0x50);
    CHECK(manager.loadFromSource(desc, chunkedFill, &retry));
    CHECK(manager.isResident(4));
    CHECK(retry.destination == reinterpret_cast<std::uint8_t*>(arena_words.data()));
}

void test_descriptor_conflict_does_not_call_source() {
    alignas(4) std::array<std::uint32_t, 32> arena_words{};
    SampleResidencyManager manager;
    CHECK(manager.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    FillContext initial = patterned(16, 0x11);
    CHECK(manager.loadFromSource(descriptor(6, 16), chunkedFill, &initial));

    FillContext conflicting = patterned(20, 0x22);
    CHECK(!manager.loadFromSource(descriptor(6, 20), chunkedFill, &conflicting));
    CHECK(manager.lastError() == ResidencyError::DescriptorMismatch);
    CHECK(conflicting.calls == 0);
    CHECK(manager.stats().descriptor_conflicts == 1);
}

}  // namespace

int main() {
    test_successful_direct_fill_commits_only_after_callback();
    test_existing_sample_reuses_without_reading_source_again();
    test_failed_partial_fill_rolls_back_span_and_visibility();
    test_descriptor_conflict_does_not_call_source();

    if (failures != 0) {
        std::fprintf(stderr, "%d sample source-load checks failed\n", failures);
        return 1;
    }
    std::printf("sample direct-source load checks passed\n");
    return 0;
}
