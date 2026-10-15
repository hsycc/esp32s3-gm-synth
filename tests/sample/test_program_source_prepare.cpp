#include "../../firmware/sample/ProgramPrepareController.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramPrepareError;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sample::SampleSource;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

class FakeSampleSource final : public SampleSource {
public:
    void define(SampleId id, std::size_t bytes, std::uint8_t seed) {
        present_[id] = true;
        descriptors_[id] = SampleDescriptor{};
        descriptors_[id].id = id;
        descriptors_[id].pcm_bytes = static_cast<std::uint32_t>(bytes);
        descriptors_[id].frames = static_cast<std::uint32_t>(bytes / 2);
        descriptors_[id].loop_start = 1;
        descriptors_[id].loop_end = static_cast<std::uint32_t>(bytes / 2 - 1);
        descriptors_[id].sample_rate = 44100;
        descriptors_[id].root_key = 60;
        descriptors_[id].flags = 1;
        bytes_[id].resize(bytes);
        for (std::size_t i = 0; i < bytes; ++i) {
            bytes_[id][i] = static_cast<std::uint8_t>(seed + i);
        }
    }

    void setFillFailure(SampleId id, bool fail) { fail_fill_[id] = fail; }

    bool descriptorFor(SampleId id, SampleDescriptor* descriptor) override {
        if (id >= present_.size() || descriptor == nullptr || !present_[id]) {
            return false;
        }
        ++descriptor_calls_[id];
        *descriptor = descriptors_[id];
        return true;
    }

    bool fillSample(SampleId id, std::uint8_t* destination, std::size_t pcm_bytes) override {
        if (id >= present_.size() || destination == nullptr || !present_[id] ||
            bytes_[id].size() != pcm_bytes) {
            return false;
        }
        ++fill_calls_[id];
        const std::size_t half = pcm_bytes / 2;
        std::memcpy(destination, bytes_[id].data(), half);
        if (fail_fill_[id]) {
            return false;
        }
        std::memcpy(destination + half, bytes_[id].data() + half, pcm_bytes - half);
        return true;
    }

    int fillCalls(SampleId id) const { return fill_calls_[id]; }
    int descriptorCalls(SampleId id) const { return descriptor_calls_[id]; }

private:
    std::array<bool, 512> present_{};
    std::array<bool, 512> fail_fill_{};
    std::array<SampleDescriptor, 512> descriptors_{};
    std::array<std::vector<std::uint8_t>, 512> bytes_{};
    std::array<int, 512> fill_calls_{};
    std::array<int, 512> descriptor_calls_{};
};

void test_prepare_loads_missing_samples_once_and_reuses_cache() {
    alignas(4) std::array<std::uint32_t, 128> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    source.define(1, 16, 0x10);
    source.define(2, 20, 0x20);
    source.define(3, 24, 0x30);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 2> program_a{{1, 2}};
    const std::array<SampleId, 2> program_b{{2, 3}};
    const std::array<SampleId, 2> program_c{{1, 3}};

    CHECK(controller.activateInitialProgram(0, 0, 8, 100, program_a.data(), program_a.size()));
    CHECK(source.fillCalls(1) == 1);
    CHECK(source.fillCalls(2) == 1);
    CHECK(residency.residentView(1).pin_count == 1);
    CHECK(residency.residentView(2).pin_count == 1);

    CHECK(controller.prepareProgram(0, 0, 16, 101, program_b.data(), program_b.size()));
    CHECK(source.fillCalls(2) == 1);
    CHECK(source.fillCalls(3) == 1);
    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(controller.acknowledgeAndRetire(0, 101).valid());

    CHECK(residency.isResident(1));
    CHECK(residency.residentView(1).pin_count == 0);
    CHECK(directory.published(1) == nullptr);

    CHECK(controller.prepareProgram(0, 0, 32, 102, program_c.data(), program_c.size()));
    CHECK(source.fillCalls(1) == 1);
    CHECK(source.fillCalls(3) == 1);

    const auto stats = controller.stats();
    CHECK(stats.samples_loaded == 3);
    CHECK(stats.sample_bytes_loaded == 60);
    CHECK(stats.samples_reused_resident >= 3);
    CHECK(stats.source_failures == 0);
}

void test_source_failure_rolls_back_ownership_but_keeps_successful_cache() {
    alignas(4) std::array<std::uint32_t, 128> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    source.define(4, 16, 0x40);
    source.define(5, 24, 0x50);
    source.setFillFailure(5, true);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 2> program{{4, 5}};
    CHECK(!controller.activateInitialProgram(0, 0, 40, 200, program.data(), program.size()));
    CHECK(controller.lastError() == ProgramPrepareError::SourceLoadFailed);
    CHECK(exchange.active(0) == nullptr);

    CHECK(residency.isResident(4));
    CHECK(residency.residentView(4).pin_count == 0);
    CHECK(directory.published(4) == nullptr);
    CHECK(!residency.isResident(5));
    CHECK(source.fillCalls(4) == 1);
    CHECK(source.fillCalls(5) == 1);

    source.setFillFailure(5, false);
    CHECK(controller.activateInitialProgram(0, 0, 40, 201, program.data(), program.size()));
    CHECK(source.fillCalls(4) == 1);
    CHECK(source.fillCalls(5) == 2);
    CHECK(residency.residentView(4).pin_count == 1);
    CHECK(residency.residentView(5).pin_count == 1);

    const auto stats = controller.stats();
    CHECK(stats.prepare_attempts == 2);
    CHECK(stats.prepare_successes == 1);
    CHECK(stats.prepare_failures == 1);
    CHECK(stats.samples_loaded == 2);
    CHECK(stats.source_failures == 1);
    CHECK(stats.samples_reused_resident >= 1);
}

void test_missing_descriptor_fails_before_arena_mutation() {
    alignas(4) std::array<std::uint32_t, 32> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 1> missing{{10}};
    CHECK(!controller.activateInitialProgram(0, 0, 8, 300, missing.data(), missing.size()));
    CHECK(controller.lastError() == ProgramPrepareError::SourceDescriptorFailed);
    CHECK(residency.stats().used_bytes == 0);
    CHECK(directory.stats().published_sample_count == 0);
}

void test_capacity_pressure_evicts_only_safe_unowned_cache() {
    // 80-byte arena. Active sample 1 uses 24 B. Failed prepares leave samples
    // 2 and 3 as two 20 B unowned cache entries. Loading sample 4 (20 B) then
    // requires one eviction; LRU must choose sample 2, not Active sample 1.
    alignas(4) std::array<std::uint32_t, 20> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    source.define(1, 24, 0x10);
    source.define(2, 20, 0x20);
    source.define(3, 20, 0x30);
    source.define(4, 20, 0x40);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 1> active_program{{1}};
    CHECK(controller.activateInitialProgram(
        0, 0, 8, 400, active_program.data(), active_program.size()));
    const auto* active_before = exchange.active(0);
    const auto* published_active = directory.published(1);

    const std::array<SampleId, 2> make_cache2{{2, 99}};
    CHECK(!controller.prepareProgram(
        1, 0, 16, 401, make_cache2.data(), make_cache2.size()));
    CHECK(residency.isResident(2));
    CHECK(residency.residentView(2).pin_count == 0);
    CHECK(directory.published(2) == nullptr);

    const std::array<SampleId, 2> make_cache3{{3, 99}};
    CHECK(!controller.prepareProgram(
        1, 0, 17, 402, make_cache3.data(), make_cache3.size()));
    CHECK(residency.isResident(3));
    CHECK(residency.residentView(3).pin_count == 0);
    CHECK(residency.residentView(2).touch_sequence < residency.residentView(3).touch_sequence);

    const std::array<SampleId, 2> next_program{{1, 4}};
    CHECK(controller.prepareProgram(
        0, 0, 24, 403, next_program.data(), next_program.size()));

    CHECK(!residency.isResident(2));  // oldest safe cache victim
    CHECK(residency.isResident(3));
    CHECK(residency.isResident(4));
    CHECK(residency.isResident(1));
    CHECK(directory.published(1) == published_active);
    CHECK(exchange.active(0) == active_before);  // not committed yet
    CHECK(exchange.ready(0) != nullptr);

    const auto stats = controller.stats();
    CHECK(stats.cache_evictions == 1);
    CHECK(stats.cache_evicted_bytes == 20);
    CHECK(stats.capacity_retries == 1);
    CHECK(stats.capacity_failures == 0);
}

void test_impossible_prepare_preserves_old_active_program() {
    // 64-byte arena. Active sample 1 owns 40 B. New sample 4 needs 32 B, so
    // even with zero cache entries the requested working set cannot coexist
    // with the old Active snapshot during Prepare.
    alignas(4) std::array<std::uint32_t, 16> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    source.define(1, 40, 0x11);
    source.define(4, 32, 0x44);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 1> active_program{{1}};
    CHECK(controller.activateInitialProgram(
        0, 0, 8, 500, active_program.data(), active_program.size()));

    const auto* active_before = exchange.active(0);
    const auto* published_before = directory.published(1);
    CHECK(active_before != nullptr);
    CHECK(residency.residentView(1).pin_count == 1);

    const std::array<SampleId, 2> impossible{{1, 4}};
    CHECK(!controller.prepareProgram(
        0, 0, 40, 501, impossible.data(), impossible.size()));
    CHECK(controller.lastError() == ProgramPrepareError::ResidencyCapacityFailed);

    CHECK(exchange.active(0) == active_before);
    CHECK(exchange.ready(0) == nullptr);
    CHECK(!exchange.transitionInFlight(0));
    CHECK(directory.published(1) == published_before);
    CHECK(residency.isResident(1));
    CHECK(residency.residentView(1).pin_count == 1);
    CHECK(!residency.isResident(4));
    CHECK(source.fillCalls(4) == 0);  // capacity rejected before storage read

    const auto stats = controller.stats();
    CHECK(stats.capacity_failures == 1);
    CHECK(stats.cache_evictions == 0);
}

void test_single_sample_larger_than_arena_fails_without_eviction_or_io() {
    alignas(4) std::array<std::uint32_t, 16> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    FakeSampleSource source;

    source.define(12, 80, 0x12);
    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &source);

    const std::array<SampleId, 1> too_large{{12}};
    CHECK(!controller.activateInitialProgram(
        0, 0, 0, 600, too_large.data(), too_large.size()));
    CHECK(controller.lastError() == ProgramPrepareError::ResidencyCapacityFailed);
    CHECK(source.fillCalls(12) == 0);
    CHECK(residency.stats().used_bytes == 0);
    CHECK(controller.stats().capacity_failures == 1);
}

}  // namespace

int main() {
    test_prepare_loads_missing_samples_once_and_reuses_cache();
    test_source_failure_rolls_back_ownership_but_keeps_successful_cache();
    test_missing_descriptor_fails_before_arena_mutation();
    test_capacity_pressure_evicts_only_safe_unowned_cache();
    test_impossible_prepare_preserves_old_active_program();
    test_single_sample_larger_than_arena_fails_without_eviction_or_io();

    if (failures != 0) {
        std::fprintf(stderr, "%d program source-prepare checks failed\n", failures);
        return 1;
    }
    std::printf("program source-prepare host checks passed\n");
    return 0;
}
