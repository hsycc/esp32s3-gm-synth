#include "gm/ProgramChangeControlService.h"
#include "gm/ProgramRuntimeAudioBlock.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sample/ProgramPrepareController.h"
#include "sample/ProgramSnapshot.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::ProgramChangeControlService;
using gm_synth::gm::ProgramChangeMailbox;
using gm_synth::gm::ProgramChangePrepareWorkspace;
using gm_synth::gm::ProgramRuntimeBlockCommitter;
using gm_synth::gm::ProgramRuntimeCommitCoordinator;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::RealtimeProgramChangeRouter;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::kGmPercussionChannel;
using gm_synth::gm::kSf2PercussionBank;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::Sf2SampleSource;
using gm_synth::storage::RandomAccessReader;
using gm_synth::voice::VoicePool;

constexpr const char* kFullBankPath =
    "assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2";
constexpr std::size_t kMiB = 1024u * 1024u;
constexpr std::size_t kSampleSlots = SampleResidencyManager::kMaxResidentSamples;

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

struct WorkspaceStorage {
    std::vector<NativeResolvedZone> native;
    std::vector<ResolvedZoneModulation> modulation;
    std::vector<gm_synth::gm::PlaybackZone> playback;
    std::vector<SampleId> dependencies;
    ProgramChangePrepareWorkspace workspace{};

    WorkspaceStorage()
        : native(256), modulation(256), playback(256), dependencies(512) {
        workspace.native_scratch = native.data();
        workspace.native_capacity = native.size();
        workspace.modulation_scratch = modulation.data();
        workspace.modulation_capacity = modulation.size();
        workspace.playback_zones = playback.data();
        workspace.playback_capacity = playback.size();
        workspace.dependencies = dependencies.data();
        workspace.dependency_capacity = dependencies.size();
        workspace.policy = PlaybackPreparePolicy::StrictQualified;
    }
};

struct DependencySet {
    std::array<bool, kSampleSlots> present{};
    std::size_t count = 0;
};

struct WorstReadyCandidate {
    std::uint8_t channel = 0;
    std::uint8_t program = 0;
    std::size_t active_plus_ready_bytes = 0;
    std::size_t candidate_dependency_count = 0;
};

struct TrialResult {
    std::size_t arena_bytes = 0;
    bool active_baseline_ready = false;
    bool ready_prepared = false;
    bool committed = false;
    std::size_t high_water_bytes = 0;
    std::size_t used_bytes = 0;
    std::uint64_t cache_evictions = 0;
    std::uint64_t cache_evicted_bytes = 0;
    std::uint64_t capacity_retries = 0;
    std::uint64_t capacity_failures = 0;
    unsigned service_status = 0;
    unsigned residency_error = 0;
};

RealtimeEvent programChange(std::uint8_t channel, std::uint8_t program) {
    RealtimeEvent event{};
    event.type = EventType::ProgramChange;
    event.channel = channel;
    event.data1 = program;
    return event;
}

bool resolveProgramDependencies(
    Sf2PdtaReader& pdta,
    const Sf2PlaybackProgramPreparer& preparer,
    WorkspaceStorage& storage,
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token,
    std::size_t* dependency_count) {
    PlaybackPrepareRequest request{};
    request.channel = channel;
    request.bank = bank;
    request.program = program;
    request.token = token;
    request.bank_generation = 1;
    request.policy = PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = storage.workspace.native_scratch;
    request.native_capacity = storage.workspace.native_capacity;
    request.modulation_scratch = storage.workspace.modulation_scratch;
    request.modulation_capacity = storage.workspace.modulation_capacity;
    request.playback_zones = storage.workspace.playback_zones;
    request.playback_capacity = storage.workspace.playback_capacity;
    request.dependencies = storage.workspace.dependencies;
    request.dependency_capacity = storage.workspace.dependency_capacity;
    request.snapshot = &storage.workspace.snapshot;

    const auto result = preparer.prepare(pdta, request);
    if (!result.ok() || result.unsupported_modulator_records != 0 ||
        result.preview_zone_count != 0 || result.dependency_count == 0) {
        return false;
    }
    *dependency_count = result.dependency_count;
    return true;
}

void addDependencies(
    DependencySet* set,
    const SampleId* dependencies,
    std::size_t dependency_count) {
    assert(set != nullptr);
    for (std::size_t i = 0; i < dependency_count; ++i) {
        const SampleId sample_id = dependencies[i];
        assert(static_cast<std::size_t>(sample_id) < set->present.size());
        if (!set->present[sample_id]) {
            set->present[sample_id] = true;
            ++set->count;
        }
    }
}

std::size_t alignedBytesForSet(Sf2SampleSource& source, const DependencySet& set) {
    std::size_t total = 0;
    for (std::size_t sample_id = 0; sample_id < set.present.size(); ++sample_id) {
        if (!set.present[sample_id]) {
            continue;
        }
        SampleDescriptor descriptor{};
        assert(source.descriptorFor(static_cast<SampleId>(sample_id), &descriptor));
        total += SampleResidencyManager::alignedBytes(descriptor.pcm_bytes);
    }
    return total;
}

std::uint8_t baselineProgram(std::uint8_t channel) {
    return channel == kGmPercussionChannel ? 0 : static_cast<std::uint8_t>(channel * 8u);
}

std::uint16_t baselineBank(std::uint8_t channel) {
    return channel == kGmPercussionChannel ? kSf2PercussionBank : 0;
}

WorstReadyCandidate discoverWorstReadyCandidate(
    Sf2PdtaReader& pdta,
    Sf2SampleSource& source,
    const Sf2PlaybackProgramPreparer& preparer,
    DependencySet* active_set,
    std::size_t* active_bytes) {
    assert(active_set != nullptr);
    assert(active_bytes != nullptr);
    std::array<WorkspaceStorage, 16> baseline_workspaces{};

    std::uint32_t token = 1000;
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        std::size_t dependency_count = 0;
        assert(resolveProgramDependencies(
            pdta,
            preparer,
            baseline_workspaces[channel],
            channel,
            baselineBank(channel),
            baselineProgram(channel),
            token++,
            &dependency_count));
        addDependencies(
            active_set,
            baseline_workspaces[channel].workspace.dependencies,
            dependency_count);
    }
    *active_bytes = alignedBytesForSet(source, *active_set);

    WorstReadyCandidate worst{};
    WorkspaceStorage candidate_workspace;
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        if (channel == kGmPercussionChannel) {
            continue;
        }
        for (std::uint16_t program = 0; program < 128; ++program) {
            std::size_t dependency_count = 0;
            assert(resolveProgramDependencies(
                pdta,
                preparer,
                candidate_workspace,
                channel,
                0,
                static_cast<std::uint8_t>(program),
                token++,
                &dependency_count));
            DependencySet combined = *active_set;
            addDependencies(
                &combined,
                candidate_workspace.workspace.dependencies,
                dependency_count);
            const std::size_t bytes = alignedBytesForSet(source, combined);
            if (bytes > worst.active_plus_ready_bytes) {
                worst.channel = channel;
                worst.program = static_cast<std::uint8_t>(program);
                worst.active_plus_ready_bytes = bytes;
                worst.candidate_dependency_count = dependency_count;
            }
        }
    }
    return worst;
}

bool activateInitialProgram(
    Sf2PdtaReader& pdta,
    const Sf2PlaybackProgramPreparer& playback_preparer,
    ProgramPrepareController& residency_preparer,
    PublishedSampleDirectory& directory,
    PlaybackProgramSnapshotExchange& playback_exchange,
    WorkspaceStorage& storage,
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token) {
    std::size_t dependency_count = 0;
    if (!resolveProgramDependencies(
            pdta,
            playback_preparer,
            storage,
            channel,
            bank,
            program,
            token,
            &dependency_count)) {
        return false;
    }
    storage.workspace.snapshot.bank_generation = directory.bankGeneration();
    if (!residency_preparer.activateInitialProgram(
            channel,
            bank,
            program,
            token,
            storage.workspace.dependencies,
            dependency_count)) {
        return false;
    }
    return playback_exchange.setInitialActive(&storage.workspace.snapshot);
}

TrialResult runCapacityTrial(std::size_t arena_bytes, const WorstReadyCandidate& worst) {
    TrialResult trial{};
    trial.arena_bytes = arena_bytes;

    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource source;
    assert(pdta.begin(reader));
    assert(source.begin(reader));

    std::vector<std::uint32_t> arena_words(arena_bytes / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    std::array<WorkspaceStorage, 16> active_workspaces{};

    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        if (!activateInitialProgram(
                pdta,
                playback_preparer,
                residency_preparer,
                directory,
                playback_exchange,
                active_workspaces[channel],
                channel,
                baselineBank(channel),
                baselineProgram(channel),
                static_cast<std::uint32_t>(5000u + channel))) {
            const auto stats = residency.stats();
            const auto prepare_stats = residency_preparer.stats();
            trial.high_water_bytes = stats.high_water_bytes;
            trial.used_bytes = stats.used_bytes;
            trial.cache_evictions = prepare_stats.cache_evictions;
            trial.cache_evicted_bytes = prepare_stats.cache_evicted_bytes;
            trial.capacity_retries = prepare_stats.capacity_retries;
            trial.capacity_failures = prepare_stats.capacity_failures;
            return trial;
        }
    }
    trial.active_baseline_ready = true;

    RealtimeProgramChangeHandoff handoff;
    ProgramChangeMailbox mailbox;
    ProgramChangeControlService service(
        handoff,
        mailbox,
        pdta,
        playback_preparer,
        residency_preparer,
        residency_exchange,
        playback_exchange);
    RealtimeProgramEventApplier events(playback_exchange);
    RealtimeProgramChangeRouter router(events, handoff);
    VoicePool voices(directory);
    ProgramRuntimeCommitCoordinator coordinator(residency_exchange, playback_exchange);
    ProgramRuntimeBlockCommitter committer(coordinator);
    WorkspaceStorage ready_workspace;

    assert(router.apply(programChange(worst.channel, worst.program), voices));
    assert(service.drainRealtimeRequests() == 1);
    const auto prepared = service.serviceChannel(worst.channel, ready_workspace.workspace);
    trial.service_status = static_cast<unsigned>(prepared.status);
    trial.residency_error = static_cast<unsigned>(prepared.residency_error);
    if (prepared.ok()) {
        trial.ready_prepared = true;
        assert(prepared.request.bank == 0);
        assert(prepared.request.program == worst.program);
        assert(coordinator.readyCompatible(worst.channel));
        assert(committer.commitReadyAtBlockBoundary() == 1);
        assert(service.acknowledgeCommitted(worst.channel).ok());
        trial.committed = true;
    }

    const auto stats = residency.stats();
    const auto prepare_stats = residency_preparer.stats();
    trial.high_water_bytes = stats.high_water_bytes;
    trial.used_bytes = stats.used_bytes;
    trial.cache_evictions = prepare_stats.cache_evictions;
    trial.cache_evicted_bytes = prepare_stats.cache_evicted_bytes;
    trial.capacity_retries = prepare_stats.capacity_retries;
    trial.capacity_failures = prepare_stats.capacity_failures;
    assert(stats.stale_reference_faults == 0);
    assert(stats.descriptor_conflicts == 0);
    assert(directory.stats().stale_release_faults == 0);
    return trial;
}

}  // namespace

int main() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource source;
    assert(pdta.begin(reader));
    assert(source.begin(reader));
    Sf2PlaybackProgramPreparer preparer(44100);

    DependencySet active_set{};
    std::size_t active_bytes = 0;
    const WorstReadyCandidate worst = discoverWorstReadyCandidate(
        pdta, source, preparer, &active_set, &active_bytes);
    assert(active_set.count > 0);
    assert(active_bytes > 4u * kMiB);
    assert(worst.active_plus_ready_bytes >= active_bytes);

    constexpr std::array<std::size_t, 7> kArenaCandidates{
        4u * kMiB,
        4u * kMiB + kMiB / 4u,
        4u * kMiB + kMiB / 2u,
        4u * kMiB + (3u * kMiB) / 4u,
        5u * kMiB,
        5u * kMiB + kMiB / 2u,
        6u * kMiB,
    };

    std::size_t first_success = 0;
    for (const std::size_t arena_bytes : kArenaCandidates) {
        const TrialResult trial = runCapacityTrial(arena_bytes, worst);
        if (trial.committed && first_success == 0) {
            first_success = arena_bytes;
        }
        std::cout
            << "GM128_CAPACITY_TRIAL arena=" << arena_bytes
            << " active=" << (trial.active_baseline_ready ? 1 : 0)
            << " ready=" << (trial.ready_prepared ? 1 : 0)
            << " committed=" << (trial.committed ? 1 : 0)
            << " high_water=" << trial.high_water_bytes
            << " used=" << trial.used_bytes
            << " evictions=" << trial.cache_evictions
            << " evicted_bytes=" << trial.cache_evicted_bytes
            << " retries=" << trial.capacity_retries
            << " capacity_failures=" << trial.capacity_failures
            << " service_status=" << trial.service_status
            << " residency_error=" << trial.residency_error
            << '\n';
    }

    assert(first_success != 0);
    assert(first_success <= 6u * kMiB);
    assert(first_success >= active_bytes);

    std::cout << "GM128_CAPACITY_ACTIVE_UNIQUE_SAMPLES=" << active_set.count << '\n';
    std::cout << "GM128_CAPACITY_ACTIVE_ALIGNED_BYTES=" << active_bytes << '\n';
    std::cout << "GM128_CAPACITY_WORST_READY_CHANNEL=" << static_cast<unsigned>(worst.channel) << '\n';
    std::cout << "GM128_CAPACITY_WORST_READY_PROGRAM=" << static_cast<unsigned>(worst.program) << '\n';
    std::cout << "GM128_CAPACITY_WORST_READY_DEPENDENCIES=" << worst.candidate_dependency_count << '\n';
    std::cout << "GM128_CAPACITY_ACTIVE_PLUS_READY_ALIGNED_BYTES=" << worst.active_plus_ready_bytes << '\n';
    std::cout << "GM128_CAPACITY_FIRST_SUCCESS_BYTES=" << first_success << '\n';
    std::cout << "Full FM GM128 capacity qualification passed.\n";
    return 0;
}
