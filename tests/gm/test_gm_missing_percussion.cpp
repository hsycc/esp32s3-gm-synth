#include "gm/MidiChannelState.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ProgramChangeControlService.h"
#include "gm/ProgramChangeMailbox.h"
#include "gm/RealtimeProgramChangeHandoff.h"
#include "sample/ProgramPrepareController.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"

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
using gm_synth::gm::ProgramChangeRequest;
using gm_synth::gm::ProgramChangeServiceStatus;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::kGmPercussionChannel;
using gm_synth::gm::kSf2PercussionBank;
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
    std::vector<gm_synth::sample::SampleId> dependencies;
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
        workspace.policy = PlaybackPreparePolicy::AllowPreview;
    }
};

void activateMelodicFallbackForChannel10(
    Sf2PdtaReader& pdta,
    Sf2PlaybackProgramPreparer& playback_preparer,
    ProgramPrepareController& residency_preparer,
    PublishedSampleDirectory& directory,
    PlaybackProgramSnapshotExchange& playback_exchange,
    WorkspaceStorage& storage) {
    PlaybackPrepareRequest request{};
    request.channel = kGmPercussionChannel;
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
    assert(playback.dependency_count != 0);
    assert(residency_preparer.activateInitialProgram(
        kGmPercussionChannel,
        0,
        0,
        100,
        storage.workspace.dependencies,
        playback.dependency_count));
    assert(playback_exchange.setInitialActive(&storage.workspace.snapshot));
}

void testMissingBank128RequestIsConsumedWithoutMutatingActiveGeneration() {
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
        residency,
        directory,
        residency_exchange,
        &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    WorkspaceStorage active;
    WorkspaceStorage candidate;
    activateMelodicFallbackForChannel10(
        pdta,
        playback_preparer,
        residency_preparer,
        directory,
        playback_exchange,
        active);

    assert(residency_exchange.active(kGmPercussionChannel)->bank == 0);
    assert(playback_exchange.active(kGmPercussionChannel)->bank == 0);

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

    ProgramChangeRequest request{};
    request.channel = kGmPercussionChannel;
    request.bank = kSf2PercussionBank;
    request.program = 0;
    request.token = 101;
    assert(handoff.publish(request));
    assert(service.drainRealtimeRequests() == 1);

    const auto result = service.serviceChannel(kGmPercussionChannel, candidate.workspace);
    assert(result.status == ProgramChangeServiceStatus::PresetUnavailable);
    assert(result.request.bank == kSf2PercussionBank);
    assert(!mailbox.hasPending(kGmPercussionChannel));

    // The failed drum lookup never publishes half of a runtime generation.
    assert(!residency_exchange.transitionInFlight(kGmPercussionChannel));
    assert(!playback_exchange.transitionInFlight(kGmPercussionChannel));
    assert(residency_exchange.active(kGmPercussionChannel)->bank == 0);
    assert(residency_exchange.active(kGmPercussionChannel)->token == 100);
    assert(playback_exchange.active(kGmPercussionChannel)->bank == 0);
    assert(playback_exchange.active(kGmPercussionChannel)->token == 100);

    const auto stats = service.stats();
    assert(stats.playback_prepare_failures == 1);
    assert(stats.preset_unavailable_drops == 1);
    assert(stats.residency_prepare_failures == 0);
    assert(stats.publish_invariant_failures == 0);
}

}  // namespace

int main() {
    testMissingBank128RequestIsConsumedWithoutMutatingActiveGeneration();
    std::cout << "GM missing-percussion transaction checks passed\n";
    return 0;
}
