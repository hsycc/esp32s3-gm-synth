#include "../../firmware/sample/ProgramPrepareController.h"
#include "real_bank_dependencies.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::PublishedVoiceRef;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::test_fixture::BankDependencyFixture;
using gm_synth::test_fixture::ProgramDependencyFixture;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

constexpr std::uint32_t kSyntheticPcmBytes = 8;

SampleDescriptor descriptor(SampleId id) {
    SampleDescriptor d{};
    d.id = id;
    d.pcm_bytes = kSyntheticPcmBytes;
    d.frames = kSyntheticPcmBytes / 2;
    d.loop_start = 1;
    d.loop_end = 3;
    d.sample_rate = 44100;
    d.root_key = 60;
    d.flags = 1;
    return d;
}

bool programContains(const ProgramDependencyFixture& program, SampleId id) {
    for (std::size_t i = 0; i < program.sample_count; ++i) {
        if (program.sample_ids[i] == id) {
            return true;
        }
    }
    return false;
}

std::uint16_t expectedPinCount(
    const BankDependencyFixture& bank,
    SampleId id,
    bool channel_zero_switched_to_program_one) {
    std::uint16_t count = 0;
    for (std::size_t channel = 0; channel < bank.program_count; ++channel) {
        const std::size_t effective_program =
            (channel_zero_switched_to_program_one && channel == 0 && bank.program_count > 1)
                ? 1
                : channel;
        if (programContains(bank.programs[effective_program], id)) {
            ++count;
        }
    }
    return count;
}

SampleId findSharedSample(const BankDependencyFixture& bank) {
    for (std::size_t i = 0; i < bank.unique_sample_count; ++i) {
        const SampleId id = bank.unique_sample_ids[i];
        if (expectedPinCount(bank, id, false) > 1) {
            return id;
        }
    }
    return static_cast<SampleId>(SampleResidencyManager::kMaxResidentSamples);
}

void validatePublishedPins(
    const BankDependencyFixture& bank,
    SampleResidencyManager& residency,
    PublishedSampleDirectory& directory,
    bool channel_zero_switched_to_program_one) {
    for (std::size_t i = 0; i < bank.unique_sample_count; ++i) {
        const SampleId id = bank.unique_sample_ids[i];
        const std::uint16_t expected =
            expectedPinCount(bank, id, channel_zero_switched_to_program_one);
        const auto view = residency.residentView(id);
        CHECK(view.valid());
        CHECK(view.pin_count == expected);
        if (expected == 0) {
            CHECK(directory.published(id) == nullptr);
        } else {
            CHECK(directory.published(id) != nullptr);
        }
    }
}

void exerciseBank(const BankDependencyFixture& bank) {
    CHECK(bank.program_count > 0);
    CHECK(bank.program_count <= ProgramSnapshotExchange::kMidiChannels);
    CHECK(bank.unique_sample_count > 0);
    CHECK(bank.unique_sample_count <= SampleResidencyManager::kMaxResidentSamples);
    CHECK(bank.greedy_aligned_pcm_bytes <= bank.all_presets_aligned_pcm_bytes);

    // Preserve the real dependency graph while making PCM intentionally tiny.
    // Byte-pressure correctness is checked from the manifest independently by
    // the generator/Python tests; this executable targets ownership semantics.
    std::vector<std::uint32_t> arena_words(bank.unique_sample_count * 2 + 16, 0);
    const std::array<std::uint8_t, kSyntheticPcmBytes> pcm{};

    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    ProgramPrepareController controller(residency, directory, exchange);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    for (std::size_t i = 0; i < bank.unique_sample_count; ++i) {
        CHECK(residency.load(
            descriptor(bank.unique_sample_ids[i]), pcm.data(), pcm.size()));
    }

    for (std::size_t channel = 0; channel < bank.program_count; ++channel) {
        const ProgramDependencyFixture& program = bank.programs[channel];
        CHECK(controller.activateInitialResidentProgram(
            static_cast<std::uint8_t>(channel),
            program.bank,
            program.program,
            static_cast<std::uint32_t>(1000 + channel),
            program.sample_ids,
            program.sample_count));
        const auto* active = exchange.active(static_cast<std::uint8_t>(channel));
        CHECK(active != nullptr);
        if (active != nullptr) {
            CHECK(active->bank == program.bank);
            CHECK(active->program == program.program);
            CHECK(active->dependency_count == program.sample_count);
        }
    }

    const auto residency_stats = residency.stats();
    const auto publication_stats = directory.stats();
    CHECK(residency_stats.resident_sample_count == bank.unique_sample_count);
    CHECK(publication_stats.published_sample_count == bank.unique_sample_count);
    validatePublishedPins(bank, residency, directory, false);

    // If the real working set shares any sample between programs, acquire one
    // through the audio publication surface to validate shared publication.
    const SampleId shared = findSharedSample(bank);
    if (shared < SampleResidencyManager::kMaxResidentSamples) {
        PublishedVoiceRef voice = directory.acquire(shared);
        CHECK(voice.valid());
        CHECK(directory.release(voice));
        CHECK(!voice.valid());
    }

    // Exercise a real Program Change on channel 0 using another program from
    // the same greedy 16-channel bank fixture. This deliberately creates two
    // channels with the same program and retires channel 0's old dependency
    // set; old-only samples must unpublish, while every still-pinned shared
    // sample must remain published.
    if (bank.program_count > 1) {
        const ProgramDependencyFixture& next = bank.programs[1];
        CHECK(controller.prepareResidentProgram(
            0,
            next.bank,
            next.program,
            9001,
            next.sample_ids,
            next.sample_count));
        CHECK(exchange.commitAtBlockBoundary(0));
        const auto ack = controller.acknowledgeAndRetire(0, 9001);
        CHECK(ack.valid());
        CHECK(ack.retired != nullptr);
        validatePublishedPins(bank, residency, directory, true);
    }

    std::printf(
        "%s: programs=%zu uniqueSamples=%zu greedyBytes=%zu allPresetsBytes=%zu\n",
        bank.name,
        bank.program_count,
        bank.unique_sample_count,
        bank.greedy_aligned_pcm_bytes,
        bank.all_presets_aligned_pcm_bytes);
}

}  // namespace

int main() {
    for (std::size_t i = 0; i < gm_synth::test_fixture::kRealBankFixtureCount; ++i) {
        exerciseBank(gm_synth::test_fixture::kRealBankFixtures[i]);
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d real-bank dependency checks failed\n", failures);
        return 1;
    }
    std::printf("real-bank dependency host checks passed\n");
    return 0;
}
