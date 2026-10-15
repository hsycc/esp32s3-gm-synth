#include "../../firmware/sample/ProgramPrepareController.h"

#include <array>
#include <cstdint>
#include <cstdio>

using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramPrepareError;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::PublishedVoiceRef;
using gm_synth::sample::ResidentSampleView;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
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
    d.loop_end = (bytes / 2) - 1;
    d.sample_rate = 44100;
    d.root_key = 60;
    d.flags = 1;
    return d;
}

void test_resident_view_tracks_control_ownership() {
    alignas(4) std::array<std::uint8_t, 128> arena{};
    std::array<std::uint8_t, 16> pcm{};
    SampleResidencyManager residency;

    CHECK(residency.begin(arena.data(), arena.size()));
    CHECK(residency.load(descriptor(4, 16), pcm.data(), pcm.size()));

    ResidentSampleView view = residency.residentView(4);
    CHECK(view.valid());
    CHECK(view.sample_id == 4);
    CHECK(view.descriptor.id == 4);
    CHECK(view.descriptor.pcm_bytes == 16);
    CHECK(view.generation != 0);
    CHECK(view.pin_count == 0);
    CHECK(view.pcm == reinterpret_cast<const std::int16_t*>(arena.data()));

    CHECK(residency.pin(4));
    view = residency.residentView(4);
    CHECK(view.pin_count == 1);
    CHECK(residency.unpin(4));
}

void test_transactional_prepare_commit_retire_and_republish_release_tail() {
    alignas(4) std::array<std::uint8_t, 256> arena{};
    std::array<std::uint8_t, 16> pcm1{};
    std::array<std::uint8_t, 16> pcm2{};
    std::array<std::uint8_t, 16> pcm3{};

    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    ProgramPrepareController controller(residency, directory, exchange);

    CHECK(residency.begin(arena.data(), arena.size()));
    CHECK(residency.load(descriptor(1, 16), pcm1.data(), pcm1.size()));
    CHECK(residency.load(descriptor(2, 16), pcm2.data(), pcm2.size()));
    CHECK(residency.load(descriptor(3, 16), pcm3.data(), pcm3.size()));

    const std::array<SampleId, 2> program_a{{1, 2}};
    const std::array<SampleId, 2> program_b{{2, 3}};
    const std::array<SampleId, 2> program_c{{1, 3}};

    CHECK(controller.activateInitialResidentProgram(
        0, 0, 8, 100, program_a.data(), program_a.size()));
    CHECK(exchange.active(0) != nullptr);
    CHECK(exchange.active(0)->program == 8);
    CHECK(directory.published(1) != nullptr);
    CHECK(directory.published(2) != nullptr);
    CHECK(residency.residentView(1).pin_count == 1);
    CHECK(residency.residentView(2).pin_count == 1);

    // Old voice starts before the program change and will outlive old pins.
    PublishedVoiceRef release_tail = directory.acquire(1);
    CHECK(release_tail.valid());
    const auto* old_record = release_tail.record;

    CHECK(controller.prepareResidentProgram(
        0, 0, 16, 101, program_b.data(), program_b.size()));
    CHECK(exchange.ready(0) != nullptr);
    CHECK(exchange.ready(0)->program == 16);
    CHECK(residency.residentView(2).pin_count == 2);
    CHECK(residency.residentView(3).pin_count == 1);

    // A second runtime transition is bounded until READY->commit->ack settles.
    CHECK(!controller.prepareResidentProgram(
        0, 0, 32, 102, program_c.data(), program_c.size()));
    CHECK(controller.lastError() == ProgramPrepareError::TransitionBusy);

    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(exchange.active(0)->program == 16);
    const auto ack_b = controller.acknowledgeAndRetire(0, 101);
    CHECK(ack_b.valid());
    CHECK(ack_b.retired != nullptr);
    CHECK(ack_b.retired->program == 8);

    // Sample 1 belongs only to the retired Program A, so it is unpublished;
    // the release-tail Voice Ref still keeps the exact record/PCM alive.
    CHECK(residency.residentView(1).pin_count == 0);
    CHECK(directory.published(1) == nullptr);
    CHECK(!directory.canReclaim(old_record));

    // Shared sample 2 remains published/pinned by active Program B.
    CHECK(residency.residentView(2).pin_count == 1);
    CHECK(directory.published(2) != nullptr);

    // Switching back to a program requiring sample 1 must republish the same
    // immutable resident record even while the old release tail is alive. No
    // reload or duplicate PCM allocation is needed.
    CHECK(controller.prepareResidentProgram(
        0, 0, 32, 102, program_c.data(), program_c.size()));
    CHECK(directory.published(1) == old_record);
    CHECK(residency.residentView(1).pin_count == 1);
    CHECK(residency.stats().resident_sample_count == 3);

    CHECK(exchange.commitAtBlockBoundary(0));
    const auto ack_c = controller.acknowledgeAndRetire(0, 102);
    CHECK(ack_c.valid());
    CHECK(ack_c.retired != nullptr);
    CHECK(ack_c.retired->program == 16);

    CHECK(residency.residentView(2).pin_count == 0);
    CHECK(directory.published(2) == nullptr);
    CHECK(residency.residentView(3).pin_count == 1);

    CHECK(directory.release(release_tail));
    CHECK(!release_tail.valid());
    CHECK(directory.published(1) == old_record);

    const auto stats = controller.stats();
    CHECK(stats.prepare_attempts == 4);
    CHECK(stats.prepare_successes == 3);
    CHECK(stats.prepare_failures == 1);
    CHECK(stats.samples_reused_published >= 2);
    CHECK(stats.retire_acks == 2);
}

void test_prepare_failure_rolls_back_partial_pins() {
    alignas(4) std::array<std::uint8_t, 128> arena{};
    std::array<std::uint8_t, 16> pcm2{};

    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    ProgramPrepareController controller(residency, directory, exchange);

    CHECK(residency.begin(arena.data(), arena.size()));
    CHECK(residency.load(descriptor(2, 16), pcm2.data(), pcm2.size()));

    const std::array<SampleId, 1> initial{{2}};
    CHECK(controller.activateInitialResidentProgram(
        0, 0, 16, 200, initial.data(), initial.size()));
    CHECK(residency.residentView(2).pin_count == 1);

    // Sample 4 is missing. Sample 2 is pinned first, then the transaction must
    // roll that temporary pin back to the original active-program ownership.
    const std::array<SampleId, 2> failing{{2, 4}};
    CHECK(!controller.prepareResidentProgram(
        0, 0, 32, 201, failing.data(), failing.size()));
    CHECK(controller.lastError() == ProgramPrepareError::NotResident);
    CHECK(exchange.ready(0) == nullptr);
    CHECK(!exchange.transitionInFlight(0));
    CHECK(residency.residentView(2).pin_count == 1);
    CHECK(directory.published(2) != nullptr);
    CHECK(!residency.residentView(4).valid());
}

void test_duplicate_dependencies_are_rejected_before_pinning() {
    alignas(4) std::array<std::uint8_t, 64> arena{};
    std::array<std::uint8_t, 16> pcm{};

    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    ProgramPrepareController controller(residency, directory, exchange);

    CHECK(residency.begin(arena.data(), arena.size()));
    CHECK(residency.load(descriptor(6, 16), pcm.data(), pcm.size()));

    const std::array<SampleId, 2> duplicate{{6, 6}};
    CHECK(!controller.activateInitialResidentProgram(
        0, 0, 8, 300, duplicate.data(), duplicate.size()));
    CHECK(controller.lastError() == ProgramPrepareError::InvalidArgument);
    CHECK(residency.residentView(6).pin_count == 0);
    CHECK(directory.published(6) == nullptr);
}

}  // namespace

int main() {
    test_resident_view_tracks_control_ownership();
    test_transactional_prepare_commit_retire_and_republish_release_tail();
    test_prepare_failure_rolls_back_partial_pins();
    test_duplicate_dependencies_are_rejected_before_pinning();

    if (failures != 0) {
        std::fprintf(stderr, "%d program prepare checks failed\n", failures);
        return 1;
    }
    std::printf("program prepare host checks passed\n");
    return 0;
}
