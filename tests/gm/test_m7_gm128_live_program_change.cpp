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

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
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
constexpr std::size_t kArenaBytes = 6u * 1024u * 1024u;
constexpr std::uint8_t kWorstReadyProgram = 122;
constexpr std::size_t kChannels = 16;
constexpr std::size_t kFramesPerBlock = 64;

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
    std::array<gm_synth::gm::PlaybackProgramKeySpan, PlaybackProgramSnapshot::kMidiKeyCount> key_spans{};
    std::vector<std::uint16_t> key_zone_indices;
    std::vector<SampleId> dependencies;
    ProgramChangePrepareWorkspace workspace{};

    WorkspaceStorage()
        : native(256), modulation(256), playback(256),
          key_zone_indices(256u * PlaybackProgramSnapshot::kMidiKeyCount), dependencies(512) {
        workspace.native_scratch = native.data();
        workspace.native_capacity = native.size();
        workspace.modulation_scratch = modulation.data();
        workspace.modulation_capacity = modulation.size();
        workspace.playback_zones = playback.data();
        workspace.playback_capacity = playback.size();
        workspace.key_spans = key_spans.data();
        workspace.key_zone_indices = key_zone_indices.data();
        workspace.key_zone_index_capacity = key_zone_indices.size();
        workspace.dependencies = dependencies.data();
        workspace.dependency_capacity = dependencies.size();
        workspace.policy = PlaybackPreparePolicy::StrictQualified;
    }
};

std::uint8_t baselineProgram(std::uint8_t channel) {
    return channel == kGmPercussionChannel ? 0 : static_cast<std::uint8_t>(channel * 8u);
}

std::uint16_t baselineBank(std::uint8_t channel) {
    return channel == kGmPercussionChannel ? kSf2PercussionBank : 0;
}

RealtimeEvent programChange(std::uint8_t channel, std::uint8_t program) {
    RealtimeEvent event{};
    event.type = EventType::ProgramChange;
    event.channel = channel;
    event.data1 = program;
    return event;
}

RealtimeEvent noteOn(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity = 100) {
    RealtimeEvent event{};
    event.type = EventType::NoteOn;
    event.channel = channel;
    event.data1 = key;
    event.data2 = velocity;
    return event;
}

bool prepareInitialProgram(
    Sf2PdtaReader& pdta,
    Sf2PlaybackProgramPreparer& playback_preparer,
    ProgramPrepareController& residency_preparer,
    PublishedSampleDirectory& directory,
    PlaybackProgramSnapshotExchange& playback_exchange,
    WorkspaceStorage& storage,
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token) {
    PlaybackPrepareRequest request{};
    request.channel = channel;
    request.bank = bank;
    request.program = program;
    request.token = token;
    request.bank_generation = directory.bankGeneration();
    request.policy = PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = storage.workspace.native_scratch;
    request.native_capacity = storage.workspace.native_capacity;
    request.modulation_scratch = storage.workspace.modulation_scratch;
    request.modulation_capacity = storage.workspace.modulation_capacity;
    request.playback_zones = storage.workspace.playback_zones;
    request.playback_capacity = storage.workspace.playback_capacity;
    request.key_spans = storage.workspace.key_spans;
    request.key_zone_indices = storage.workspace.key_zone_indices;
    request.key_zone_index_capacity = storage.workspace.key_zone_index_capacity;
    request.dependencies = storage.workspace.dependencies;
    request.dependency_capacity = storage.workspace.dependency_capacity;
    request.snapshot = &storage.workspace.snapshot;

    const auto playback = playback_preparer.prepare(pdta, request);
    if (!playback.ok() || playback.dependency_count == 0 || playback.zone_count == 0 ||
        playback.unsupported_modulator_records != 0 || playback.preview_zone_count != 0) {
        return false;
    }
    if (!residency_preparer.activateInitialProgram(
            channel,
            bank,
            program,
            token,
            storage.workspace.dependencies,
            playback.dependency_count)) {
        return false;
    }
    return playback_exchange.setInitialActive(&storage.workspace.snapshot);
}

}  // namespace

int main() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());

    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    std::vector<std::uint32_t> arena_words(kArenaBytes / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    std::array<WorkspaceStorage, kChannels> active_workspaces{};

    for (std::uint8_t channel = 0; channel < kChannels; ++channel) {
        assert(prepareInitialProgram(
            pdta,
            playback_preparer,
            residency_preparer,
            directory,
            playback_exchange,
            active_workspaces[channel],
            channel,
            baselineBank(channel),
            baselineProgram(channel),
            static_cast<std::uint32_t>(1000u + channel)));
    }

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

    const auto starts_before = events.stats().voices_started;
    const auto failures_before = events.stats().voice_start_failures;
    for (std::uint8_t channel = 0; channel < kChannels; ++channel) {
        const std::uint8_t key = channel == kGmPercussionChannel ? 36 : 60;
        assert(router.apply(noteOn(channel, key), voices));
    }
    assert(events.stats().voices_started >= starts_before + kChannels);
    assert(events.stats().voice_start_failures == failures_before);
    assert(voices.activeVoiceCount() >= kChannels);

    std::array<std::int32_t, kFramesPerBlock * 2u> mix{};
    voices.renderAdd(mix.data(), kFramesPerBlock);

    const auto before_change = directory.stats();
    assert(before_change.live_voice_refs >= kChannels);
    assert(before_change.acquire_misses == 0);

    // Exercise latest-wins Program Change while all 16 channels are sounding.
    // Channel 0 finishes on program 122, the previously discovered worst
    // Active+READY candidate. Other channels remain active and unchanged.
    for (std::uint16_t program = 0; program <= kWorstReadyProgram; ++program) {
        assert(router.apply(programChange(0, static_cast<std::uint8_t>(program)), voices));
    }
    assert(service.drainRealtimeRequests() == 1);

    gm_synth::gm::ProgramChangeRequest pending{};
    assert(mailbox.peek(0, &pending));
    assert(pending.bank == 0);
    assert(pending.program == kWorstReadyProgram);

    const auto prepared = service.serviceChannel(0, ready_workspace.workspace);
    assert(prepared.ok());
    assert(prepared.request.bank == 0);
    assert(prepared.request.program == kWorstReadyProgram);
    assert(coordinator.readyCompatible(0));

    const auto after_prepare = directory.stats();
    assert(after_prepare.live_voice_refs >= before_change.live_voice_refs);
    assert(after_prepare.acquire_misses == 0);
    assert(voices.activeVoiceCount() >= kChannels);

    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(service.acknowledgeCommitted(0).ok());
    assert(playback_exchange.active(0) != nullptr);
    assert(playback_exchange.active(0)->bank == 0);
    assert(playback_exchange.active(0)->program == kWorstReadyProgram);
    for (std::uint8_t channel = 1; channel < kChannels; ++channel) {
        const auto* snapshot = playback_exchange.active(channel);
        assert(snapshot != nullptr);
        assert(snapshot->bank == baselineBank(channel));
        assert(snapshot->program == baselineProgram(channel));
    }

    // The old channel-0 Voice still owns its published sample after the old
    // program is retired. Rendering after commit verifies that retirement did
    // not reclaim live PCM beneath any of the 16 sounding Voices.
    mix.fill(0);
    voices.renderAdd(mix.data(), kFramesPerBlock);
    const auto after_commit_render = directory.stats();
    assert(after_commit_render.live_voice_refs >= kChannels);
    assert(after_commit_render.acquire_misses == 0);
    assert(after_commit_render.stale_release_faults == 0);
    assert(voices.activeVoiceCount() >= kChannels);

    // A new NoteOn must resolve through the newly committed program while the
    // pre-change Voice and the other 15 channels are still alive.
    const auto starts_before_new = events.stats().voices_started;
    const auto failures_before_new = events.stats().voice_start_failures;
    assert(router.apply(noteOn(0, 60, 110), voices));
    assert(events.stats().voices_started > starts_before_new);
    assert(events.stats().voice_start_failures == failures_before_new);
    assert(voices.activeVoiceCount() >= kChannels + 1u);

    mix.fill(0);
    voices.renderAdd(mix.data(), kFramesPerBlock);
    const auto live_after_new = directory.stats();
    assert(live_after_new.live_voice_refs >= kChannels + 1u);
    assert(live_after_new.acquire_misses == 0);

    voices.allSoundOff();
    assert(voices.activeVoiceCount() == 0);

    const auto final_publication = directory.stats();
    const auto final_residency = residency.stats();
    const auto prepare_stats = residency_preparer.stats();
    assert(final_publication.live_voice_refs == 0);
    assert(final_publication.active_acquirers == 0);
    assert(final_publication.acquire_misses == 0);
    assert(final_publication.stale_release_faults == 0);
    assert(final_publication.publish_conflicts == 0);
    assert(final_residency.stale_reference_faults == 0);
    assert(final_residency.descriptor_conflicts == 0);

    std::cout << "M7_LIVE_PROGRAM_CHANGE"
              << " active_before=" << before_change.live_voice_refs
              << " active_after_prepare=" << after_prepare.live_voice_refs
              << " active_after_commit=" << after_commit_render.live_voice_refs
              << " active_after_new=" << live_after_new.live_voice_refs
              << " final_refs=" << final_publication.live_voice_refs
              << " final_program=" << static_cast<unsigned>(kWorstReadyProgram)
              << " arena_high_water=" << final_residency.high_water_bytes
              << " capacity_retries=" << prepare_stats.capacity_retries
              << " cache_evictions=" << prepare_stats.cache_evictions
              << " acquire_misses=" << final_publication.acquire_misses
              << " stale_release_faults=" << final_publication.stale_release_faults
              << " stale_residency_faults=" << final_residency.stale_reference_faults
              << '\n';
    std::cout << "M7 live 16-channel Program Change ownership qualification passed.\n";
    return 0;
}
