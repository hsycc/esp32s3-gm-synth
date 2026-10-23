#include "sample/ProgramPrepareController.h"
#include "sample/ProgramSnapshot.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sample/SampleSource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::PublishedVoiceRef;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sample::SampleSource;

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

constexpr SampleId kIdA = 900;
constexpr SampleId kIdB = kIdA + PublishedSampleDirectory::kMaxSamples;
constexpr SampleId kIdC = kIdB + PublishedSampleDirectory::kMaxSamples;

SampleDescriptor makeDescriptor(SampleId id, std::uint32_t bytes) {
    SampleDescriptor descriptor{};
    descriptor.id = id;
    descriptor.pcm_bytes = bytes;
    descriptor.frames = bytes / 2u;
    descriptor.loop_start = 1;
    descriptor.loop_end = descriptor.frames - 1u;
    descriptor.sample_rate = 44100;
    descriptor.root_key = 60;
    descriptor.flags = 1;
    return descriptor;
}

class HighIdSource final : public SampleSource {
public:
    bool descriptorFor(SampleId id, SampleDescriptor* descriptor) override {
        if (descriptor == nullptr) {
            return false;
        }
        if (id == kIdA) {
            *descriptor = makeDescriptor(id, 16);
            return true;
        }
        if (id == kIdB) {
            *descriptor = makeDescriptor(id, 20);
            return true;
        }
        if (id == kIdC) {
            *descriptor = makeDescriptor(id, 24);
            return true;
        }
        return false;
    }

    bool fillSample(SampleId id, std::uint8_t* destination, std::size_t pcm_bytes) override {
        SampleDescriptor descriptor{};
        if (destination == nullptr || !descriptorFor(id, &descriptor) ||
            descriptor.pcm_bytes != pcm_bytes) {
            return false;
        }
        for (std::size_t i = 0; i < pcm_bytes; ++i) {
            destination[i] = static_cast<std::uint8_t>((id + i) & 0xffu);
        }
        return true;
    }
};

void test_residency_enumerates_arbitrary_sample_ids() {
    alignas(4) std::array<std::uint8_t, 128> arena{};
    std::array<std::uint8_t, 16> pcm_a{};
    std::array<std::uint8_t, 20> pcm_b{};
    SampleResidencyManager residency;
    CHECK(residency.begin(arena.data(), arena.size()));
    CHECK(residency.load(makeDescriptor(kIdA, pcm_a.size()), pcm_a.data(), pcm_a.size()));
    CHECK(residency.load(makeDescriptor(kIdB, pcm_b.size()), pcm_b.data(), pcm_b.size()));
    CHECK(residency.residentView(kIdA).valid());
    CHECK(residency.residentView(kIdB).valid());

    bool found_a = false;
    bool found_b = false;
    for (std::size_t index = 0; index < SampleResidencyManager::kMaxResidentSamples; ++index) {
        const auto view = residency.residentViewAt(index);
        if (!view.valid()) {
            continue;
        }
        found_a = found_a || view.sample_id == kIdA;
        found_b = found_b || view.sample_id == kIdB;
    }
    CHECK(found_a);
    CHECK(found_b);
}

void initRecord(
    ResidentRecord& record,
    SampleId id,
    const std::int16_t* pcm,
    std::uint32_t bytes,
    std::uint32_t generation) {
    record.bank_generation = 1;
    record.sample_generation = generation;
    record.pcm = pcm;
    record.descriptor = makeDescriptor(id, bytes);
    record.voice_refs.store(0, std::memory_order_release);
}

void test_publication_hash_handles_high_id_collisions() {
    PublishedSampleDirectory directory;
    alignas(4) std::array<std::int16_t, 16> pcm_a{};
    alignas(4) std::array<std::int16_t, 16> pcm_b{};
    ResidentRecord a{};
    ResidentRecord b{};
    initRecord(a, kIdA, pcm_a.data(), 16, 1);
    initRecord(b, kIdB, pcm_b.data(), 16, 2);

    // kIdA and kIdB differ by exactly the hash table size and intentionally
    // collide in the pointer-only publication hash.
    CHECK(directory.publish(kIdA, &a));
    CHECK(directory.publish(kIdB, &b));
    CHECK(directory.published(kIdA) == &a);
    CHECK(directory.published(kIdB) == &b);

    PublishedVoiceRef ref_a = directory.acquire(kIdA);
    PublishedVoiceRef ref_b = directory.acquire(kIdB);
    CHECK(ref_a.valid());
    CHECK(ref_b.valid());
    CHECK(ref_a.record == &a);
    CHECK(ref_b.record == &b);

    CHECK(directory.unpublish(kIdA) == &a);
    CHECK(!directory.acquire(kIdA).valid());
    CHECK(directory.published(kIdB) == &b);
    CHECK(directory.release(ref_a));
    CHECK(directory.release(ref_b));
    CHECK(directory.canReclaim(&a));
    CHECK(directory.unpublish(kIdB) == &b);
    CHECK(directory.canReclaim(&b));
}

void test_program_prepare_keeps_512_resident_limit_but_accepts_high_ids() {
    alignas(4) std::array<std::uint32_t, 128> arena_words{};
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    HighIdSource source;
    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    ProgramPrepareController controller(residency, directory, exchange, &source);
    const std::array<SampleId, 2> first{{kIdA, kIdB}};
    CHECK(controller.activateInitialProgram(0, 0, 1, 100, first.data(), first.size()));
    CHECK(directory.published(kIdA) != nullptr);
    CHECK(directory.published(kIdB) != nullptr);

    PublishedVoiceRef tail_a = directory.acquire(kIdA);
    PublishedVoiceRef tail_b = directory.acquire(kIdB);
    CHECK(tail_a.valid());
    CHECK(tail_b.valid());

    const std::array<SampleId, 1> next{{kIdC}};
    CHECK(controller.prepareProgram(0, 0, 2, 101, next.data(), next.size()));
    CHECK(directory.published(kIdC) != nullptr);
    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(controller.acknowledgeAndRetire(0, 101).valid());

    // Retiring old SampleIds disappear from new acquisitions but their exact
    // records/PCM remain alive until the old Voice refs are released.
    CHECK(directory.published(kIdA) == nullptr);
    CHECK(directory.published(kIdB) == nullptr);
    CHECK(tail_a.record != nullptr && tail_a.record->pcm != nullptr);
    CHECK(tail_b.record != nullptr && tail_b.record->pcm != nullptr);
    CHECK(directory.release(tail_a));
    CHECK(directory.release(tail_b));

    PublishedVoiceRef current = directory.acquire(kIdC);
    CHECK(current.valid());
    CHECK(directory.release(current));

    const auto stats = directory.stats();
    CHECK(stats.live_voice_refs == 0);
    CHECK(stats.stale_release_faults == 0);
    CHECK(stats.publish_conflicts == 0);
}

}  // namespace

int main() {
    test_residency_enumerates_arbitrary_sample_ids();
    test_publication_hash_handles_high_id_collisions();
    test_program_prepare_keeps_512_resident_limit_but_accepts_high_ids();

    if (failures != 0) {
        std::fprintf(stderr, "%d large SampleId checks failed\n", failures);
        return 1;
    }
    std::printf("large SampleId host checks passed\n");
    return 0;
}
