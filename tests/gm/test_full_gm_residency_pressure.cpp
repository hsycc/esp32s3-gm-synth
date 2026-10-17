#include "sample/ProgramPrepareController.h"
#include "sample/ProgramSnapshot.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::PlaybackPrepareStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::Sf2SampleSource;
using gm_synth::storage::RandomAccessReader;

constexpr std::size_t kProductPcmArenaBytes = 4u * 1024u * 1024u;

class FileReader final : public RandomAccessReader {
public:
    explicit FileReader(const char* path) : file_(path, std::ios::binary) {
        if (!file_) {
            return;
        }
        file_.seekg(0, std::ios::end);
        const std::streamoff end = file_.tellg();
        if (end >= 0) {
            size_ = static_cast<std::uint64_t>(end);
        }
        file_.clear();
    }

    bool valid() const { return file_.is_open() && size_ != 0; }
    std::uint64_t size() const override { return size_; }

    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        if (!valid() || destination == nullptr || offset > size_ || bytes > size_ - offset) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) {
            return false;
        }
        file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
        return file_.gcount() == static_cast<std::streamsize>(bytes);
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

struct PlaybackScratch {
    std::vector<NativeResolvedZone> native{PlaybackProgramSnapshot::kMaxZones};
    std::vector<ResolvedZoneModulation> modulation{PlaybackProgramSnapshot::kMaxZones};
    std::vector<PlaybackZone> playback{PlaybackProgramSnapshot::kMaxZones};
    std::vector<SampleId> dependencies{gm_synth::sample::ProgramSnapshot::kMaxDependencies};
    PlaybackProgramSnapshot snapshot{};
};

struct ProgramDependencies {
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::vector<SampleId> samples;
    std::size_t aligned_pcm_bytes = 0;
};

ProgramDependencies resolveDependencies(
    Sf2PdtaReader& pdta,
    Sf2PlaybackProgramPreparer& preparer,
    Sf2SampleSource& sample_source,
    PlaybackScratch& scratch,
    std::uint16_t bank,
    std::uint8_t program) {
    PlaybackPrepareRequest request{};
    request.channel = bank == 128 ? 9 : 0;
    request.bank = bank;
    request.program = program;
    request.token = static_cast<std::uint32_t>(program) + (bank == 128 ? 1000u : 1u);
    request.bank_generation = 1;
    request.policy = PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = scratch.native.data();
    request.native_capacity = scratch.native.size();
    request.modulation_scratch = scratch.modulation.data();
    request.modulation_capacity = scratch.modulation.size();
    request.playback_zones = scratch.playback.data();
    request.playback_capacity = scratch.playback.size();
    request.dependencies = scratch.dependencies.data();
    request.dependency_capacity = scratch.dependencies.size();
    request.snapshot = &scratch.snapshot;

    const auto result = preparer.prepare(pdta, request);
    assert(result.status == PlaybackPrepareStatus::Ok);
    assert(result.dependency_count > 0);
    assert(result.unsupported_modulator_records == 0);
    assert(result.preview_zone_count == 0);

    ProgramDependencies output{};
    output.bank = bank;
    output.program = program;
    output.samples.assign(
        scratch.dependencies.begin(),
        scratch.dependencies.begin() + static_cast<std::ptrdiff_t>(result.dependency_count));
    std::sort(output.samples.begin(), output.samples.end());
    output.samples.erase(std::unique(output.samples.begin(), output.samples.end()), output.samples.end());

    for (const SampleId sample_id : output.samples) {
        SampleDescriptor descriptor{};
        assert(sample_source.descriptorFor(sample_id, &descriptor));
        output.aligned_pcm_bytes += SampleResidencyManager::alignedBytes(descriptor.pcm_bytes);
    }
    return output;
}

std::size_t unionBytes(
    const ProgramDependencies& left,
    const ProgramDependencies& right,
    const std::vector<std::size_t>& sample_bytes,
    std::size_t* unique_sample_count) {
    std::vector<bool> seen(sample_bytes.size(), false);
    std::size_t bytes = 0;
    std::size_t count = 0;
    const auto add = [&](const std::vector<SampleId>& samples) {
        for (const SampleId sample_id : samples) {
            assert(static_cast<std::size_t>(sample_id) < sample_bytes.size());
            if (seen[sample_id]) {
                continue;
            }
            seen[sample_id] = true;
            bytes += sample_bytes[sample_id];
            ++count;
        }
    };
    add(left.samples);
    add(right.samples);
    if (unique_sample_count != nullptr) {
        *unique_sample_count = count;
    }
    return bytes;
}

void testExactWorstFullGmPairFitsRealFourMiBArena() {
    FileReader reader("assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2");
    assert(reader.valid());

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    Sf2PlaybackProgramPreparer playback_preparer(44100);
    PlaybackScratch scratch;
    std::vector<ProgramDependencies> programs;
    programs.reserve(129);
    for (std::uint16_t program = 0; program < 128; ++program) {
        programs.push_back(resolveDependencies(
            pdta,
            playback_preparer,
            sample_source,
            scratch,
            0,
            static_cast<std::uint8_t>(program)));
    }
    programs.push_back(resolveDependencies(
        pdta, playback_preparer, sample_source, scratch, 128, 0));
    assert(programs.size() == 129);

    std::vector<std::size_t> sample_bytes(sample_source.sampleCount(), 0);
    for (std::size_t sample_id = 0; sample_id < sample_source.sampleCount(); ++sample_id) {
        SampleDescriptor descriptor{};
        assert(sample_source.descriptorFor(static_cast<SampleId>(sample_id), &descriptor));
        sample_bytes[sample_id] = SampleResidencyManager::alignedBytes(descriptor.pcm_bytes);
    }

    std::size_t worst_left = 0;
    std::size_t worst_right = 1;
    std::size_t worst_bytes = 0;
    std::size_t worst_unique_samples = 0;
    for (std::size_t left = 0; left + 1 < programs.size(); ++left) {
        for (std::size_t right = left + 1; right < programs.size(); ++right) {
            std::size_t unique_samples = 0;
            const std::size_t bytes = unionBytes(
                programs[left], programs[right], sample_bytes, &unique_samples);
            if (bytes > worst_bytes) {
                worst_bytes = bytes;
                worst_left = left;
                worst_right = right;
                worst_unique_samples = unique_samples;
            }
        }
    }

    const ProgramDependencies& active = programs[worst_left];
    const ProgramDependencies& ready = programs[worst_right];
    std::cout << "full GM exact C++ active+READY pressure: bank=" << active.bank
              << " program=" << static_cast<unsigned>(active.program)
              << " + bank=" << ready.bank
              << " program=" << static_cast<unsigned>(ready.program)
              << " -> " << worst_bytes << " bytes, unique_samples="
              << worst_unique_samples << ", arena=" << kProductPcmArenaBytes
              << ", margin=" << static_cast<std::int64_t>(kProductPcmArenaBytes) -
                     static_cast<std::int64_t>(worst_bytes)
              << '\n';

    assert(worst_bytes <= kProductPcmArenaBytes);

    std::vector<std::uint32_t> arena_words(
        kProductPcmArenaBytes / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController controller(residency, directory, exchange, &sample_source);

    assert(controller.activateInitialProgram(
        0,
        active.bank,
        active.program,
        100,
        active.samples.data(),
        active.samples.size()));
    assert(exchange.active(0) != nullptr);
    assert(exchange.active(0)->bank == active.bank);
    assert(exchange.active(0)->program == active.program);

    const auto active_stats = residency.stats();
    assert(active_stats.used_bytes == active.aligned_pcm_bytes);
    assert(active_stats.used_bytes <= kProductPcmArenaBytes);
    assert(active_stats.allocation_failures == 0);

    assert(controller.prepareProgram(
        0,
        ready.bank,
        ready.program,
        101,
        ready.samples.data(),
        ready.samples.size()));
    assert(exchange.ready(0) != nullptr);
    assert(exchange.ready(0)->bank == ready.bank);
    assert(exchange.ready(0)->program == ready.program);

    // Before commit/ACK both generations must coexist. This is the actual
    // product-capacity point represented by the exact all-pairs analyzer gate.
    const auto ready_stats = residency.stats();
    assert(ready_stats.capacity_bytes == kProductPcmArenaBytes);
    assert(ready_stats.used_bytes == worst_bytes);
    assert(ready_stats.high_water_bytes == worst_bytes);
    assert(ready_stats.pinned_sample_count == worst_unique_samples);
    assert(ready_stats.allocation_failures == 0);
    const auto prepare_stats = controller.stats();
    assert(prepare_stats.capacity_failures == 0);
    assert(prepare_stats.prepare_failures == 0);

    assert(exchange.commitAtBlockBoundary(0));
    assert(exchange.active(0) != nullptr);
    assert(exchange.active(0)->bank == ready.bank);
    assert(exchange.active(0)->program == ready.program);
    const auto ack = controller.acknowledgeAndRetire(0, 101);
    assert(ack.valid());
    assert(!exchange.transitionInFlight(0));

    const auto final_stats = controller.stats();
    assert(final_stats.retire_acks == 1);
    assert(final_stats.capacity_failures == 0);
}

}  // namespace

int main() {
    testExactWorstFullGmPairFitsRealFourMiBArena();
    std::cout << "Full GM 4 MiB residency pressure checks passed\n";
    return 0;
}
