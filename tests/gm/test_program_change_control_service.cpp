#include "gm/ProgramChangeControlService.h"
#include "gm/ProgramRuntimeAudioBlock.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sample/ProgramPrepareController.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"
#include "voice/VoiceRenderer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::ProgramChangeControlService;
using gm_synth::gm::ProgramChangeMailbox;
using gm_synth::gm::ProgramChangePrepareWorkspace;
using gm_synth::gm::ProgramChangeServiceStatus;
using gm_synth::gm::ProgramRuntimeBlockCommitter;
using gm_synth::gm::ProgramRuntimeCommitCoordinator;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::RealtimeProgramChangeRouter;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
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
    std::vector<NativeResolvedZone> native{256};
    std::vector<ResolvedZoneModulation> modulation{256};
    std::vector<gm_synth::gm::PlaybackZone> playback{256};
    std::vector<gm_synth::sample::SampleId> dependencies{512};
    ProgramChangePrepareWorkspace workspace{};

    WorkspaceStorage() {
        workspace.native_scratch = native.data();
        workspace.native_capacity = native.size();
        workspace.modulation_scratch = modulation.data();
        workspace.modulation_capacity = modulation.size();
        workspace.playback_zones = playback.data();
        workspace.playback_capacity = playback.size();
        workspace.dependencies = dependencies.data();
        workspace.dependency_capacity = dependencies.size();
        workspace.policy = PlaybackPreparePolicy::AllowPreview;
    }
};

RealtimeEvent programChange(std::uint8_t channel, std::uint8_t program) {
    RealtimeEvent event{};
    event.type = EventType::ProgramChange;
    event.channel = channel;
    event.data1 = program;
    return event;
}

void prepareInitialProgram(
    Sf2PdtaReader& pdta,
    Sf2PlaybackProgramPreparer& playback_preparer,
    ProgramPrepareController& residency_preparer,
    PublishedSampleDirectory& directory,
    PlaybackProgramSnapshotExchange& playback_exchange,
    WorkspaceStorage& storage) {
    PlaybackPrepareRequest request{};
    request.channel = 0;
    request.bank = 0;
    request.program = 0;
    request.token = 100;
    request.bank_generation = directory.bankGeneration();
    request.policy = PlaybackPreparePolicy::AllowPreview;
    request.native_scratch = storage.workspace.native_scratch;
    request.native_capacity = storage.workspace.native_capacity;
    request.modulation_scratch = storage.workspace.modulation_scratch;
    request.modulation_capacity = storage.workspace.modulation_capacity;
    request.playback_zones = storage.workspace.playback_zones;
    request.playback_capacity = storage.workspace.playback_capacity;
    request.dependencies = storage.workspace.dependencies;
    request.dependency_capacity = storage.workspace.dependency_capacity;
    request.snapshot = &storage.workspace.snapshot;

    const auto playback = playback_preparer.prepare(pdta, request);
    assert(playback.ok());
    assert(playback.zone_count == 1);
    assert(playback.dependency_count == 1);
    assert(residency_preparer.activateInitialProgram(
        0,
        0,
        0,
        100,
        storage.workspace.dependencies,
        playback.dependency_count));
    assert(playback_exchange.setInitialActive(&storage.workspace.snapshot));
}

void testRealBankProgramChangeTransactionAndRapidCoalescing() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    // Program 0/1/3 in this checked-in subset each reference roughly 128 KiB,
    // so a 1 MiB host arena leaves ample room for active + READY ownership.
    std::vector<std::uint32_t> arena_words((1024u * 1024u) / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    ProgramPrepareController residency_preparer(
        residency,
        directory,
        residency_exchange,
        &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);

    WorkspaceStorage a;
    WorkspaceStorage b;
    prepareInitialProgram(
        pdta,
        playback_preparer,
        residency_preparer,
        directory,
        playback_exchange,
        a);

    ProgramRuntimeCommitCoordinator commit_coordinator(residency_exchange, playback_exchange);
    assert(commit_coordinator.activeCompatible(0));
    assert(playback_exchange.active(0)->program == 0);

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

    assert(router.apply(programChange(0, 1), voices));
    assert(service.drainRealtimeRequests() == 1);
    const auto prepared = service.serviceChannel(0, b.workspace);
    assert(prepared.ok());
    assert(prepared.request.bank == 0);
    assert(prepared.request.program == 1);
    assert(prepared.playback_prepare.zone_count == 1);
    assert(commit_coordinator.readyCompatible(0));
    assert(playback_exchange.active(0)->program == 0);

    ProgramRuntimeBlockCommitter committer(commit_coordinator);
    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(playback_exchange.active(0)->program == 1);
    assert(residency_exchange.active(0)->program == 1);
    assert(commit_coordinator.activeCompatible(0));

    const auto ack = service.acknowledgeCommitted(0);
    assert(ack.ok());
    assert(ack.token == prepared.request.token);
    assert(ack.retired_playback == &a.workspace.snapshot);
    assert(!residency_exchange.transitionInFlight(0));
    assert(!playback_exchange.transitionInFlight(0));

    // A retired playback workspace becomes reusable only after the paired ACK.
    // Rapid Program Change traffic is coalesced before the next expensive
    // control-domain prepare, so program 2 is never loaded when program 3 wins.
    assert(router.apply(programChange(0, 2), voices));
    assert(router.apply(programChange(0, 3), voices));
    assert(service.drainRealtimeRequests() == 1);

    gm_synth::gm::ProgramChangeRequest pending{};
    assert(mailbox.peek(0, &pending));
    assert(pending.program == 3);

    const auto prepared_latest = service.serviceChannel(0, a.workspace);
    assert(prepared_latest.ok());
    assert(prepared_latest.request.program == 3);
    assert(commit_coordinator.readyCompatible(0));
    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(playback_exchange.active(0)->program == 3);
    assert(residency_exchange.active(0)->program == 3);
    const auto ack_latest = service.acknowledgeCommitted(0);
    assert(ack_latest.ok());
    assert(ack_latest.retired_playback == &b.workspace.snapshot);

    const auto stats = service.stats();
    assert(stats.realtime_requests_drained == 2);
    assert(stats.service_successes == 2);
    assert(stats.playback_prepare_failures == 0);
    assert(stats.residency_prepare_failures == 0);
    assert(stats.publish_invariant_failures == 0);
    assert(stats.commit_acks == 2);
}

void testBusyTransitionDefersLatestRequestWithoutDroppingIt() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    std::vector<std::uint32_t> arena_words((1024u * 1024u) / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    WorkspaceStorage a;
    WorkspaceStorage b;
    prepareInitialProgram(
        pdta, playback_preparer, residency_preparer, directory, playback_exchange, a);

    RealtimeProgramChangeHandoff handoff;
    ProgramChangeMailbox mailbox;
    ProgramChangeControlService service(
        handoff, mailbox, pdta, playback_preparer, residency_preparer,
        residency_exchange, playback_exchange);
    RealtimeProgramEventApplier events(playback_exchange);
    RealtimeProgramChangeRouter router(events, handoff);
    VoicePool voices(directory);

    assert(router.apply(programChange(0, 1), voices));
    assert(service.drainRealtimeRequests() == 1);
    assert(service.serviceChannel(0, b.workspace).ok());

    // A newer request may arrive while the first READY pair waits for the next
    // audio boundary. It stays pending instead of mutating the in-flight pair.
    assert(router.apply(programChange(0, 3), voices));
    assert(service.drainRealtimeRequests() == 1);
    const auto busy = service.serviceChannel(0, a.workspace);
    assert(busy.status == ProgramChangeServiceStatus::TransitionBusy);
    gm_synth::gm::ProgramChangeRequest pending{};
    assert(mailbox.peek(0, &pending));
    assert(pending.program == 3);

    ProgramRuntimeCommitCoordinator coordinator(residency_exchange, playback_exchange);
    ProgramRuntimeBlockCommitter committer(coordinator);
    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(service.acknowledgeCommitted(0).ok());

    assert(service.serviceChannel(0, a.workspace).ok());
    assert(mailbox.hasPending(0) == false);
}

}  // namespace

int main() {
    testRealBankProgramChangeTransactionAndRapidCoalescing();
    testBusyTransitionDefersLatestRequestWithoutDroppingIt();
    std::cout << "Program Change control-service integration checks passed\n";
    return 0;
}
