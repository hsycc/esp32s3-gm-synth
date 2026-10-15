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
    CHECK(source.fillCalls(2) == 1);  // already resident
    CHECK(source.fillCalls(3) == 1);
    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(controller.acknowledgeAndRetire(0, 101).valid());

    // Sample 1 is no longer pinned/published, but remains an unpinned resident
    // cache entry. Switching back must not invoke storage again.
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

    // Sample 4 completed before sample 5 failed. Its temporary program pin and
    // publication are rolled back, but the PCM remains as an unpinned cache.
    CHECK(residency.isResident(4));
    CHECK(residency.residentView(4).pin_count == 0);
    CHECK(directory.published(4) == nullptr);
    CHECK(!residency.isResident(5));
    CHECK(source.fillCalls(4) == 1);
    CHECK(source.fillCalls(5) == 1);

    source.setFillFailure(5, false);
    CHECK(controller.activateInitialProgram(0, 0, 40, 201, program.data(), program.size()));
    CHECK(source.fillCalls(4) == 1);  // reused cached PCM
    CHECK(source.fillCalls(5) == 2);  // only failed sample reread
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

}  // namespace

int main() {
    test_prepare_loads_missing_samples_once_and_reuses_cache();
    test_source_failure_rolls_back_ownership_but_keeps_successful_cache();
    test_missing_descriptor_fails_before_arena_mutation();

    if (failures != 0) {
        std::fprintf(stderr, "%d program source-prepare checks failed\n", failures);
        return 1;
    }
    std::printf("program source-prepare host checks passed\n");
    return 0;
}
