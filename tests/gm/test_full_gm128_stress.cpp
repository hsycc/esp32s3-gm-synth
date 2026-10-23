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
#include <utility>
#include <vector>

namespace {

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
constexpr std::size_t kFourMiB = 4u * 1024u * 1024u;
constexpr std::size_t kSixteenMiB = 16u * 1024u * 1024u;

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
        workspace.policy = PlaybackPreparePolicy::StrictQualified;
    }
};

RealtimeEvent programChange(std::uint8_t channel, std::uint8_t program) {
    RealtimeEvent event{};
    event.type = EventType::ProgramChange;
    event.channel = channel;
    event.data1 = program;
    return event;
}

RealtimeEvent controlChange(
    std::uint8_t channel,
    std::uint8_t controller,
    std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = channel;
    event.data1 = controller;
    event.data2 = value;
    return event;
}

RealtimeEvent noteOn(
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity = 96) {
    RealtimeEvent event{};
    event.type = EventType::NoteOn;
    event.channel = channel;
    event.data1 = key;
    event.data2 = velocity;
    return event;
}

RealtimeEvent noteOff(std::uint8_t channel, std::uint8_t key) {
    RealtimeEvent event{};
    event.type = EventType::NoteOff;
    event.channel = channel;
    event.data1 = key;
    return event;
}

RealtimeEvent pitchBend(std::uint8_t channel, std::uint16_t value) {
    RealtimeEvent event{};
    event.type = EventType::PitchBend;
    event.channel = channel;
    event.value = value;
    return event;
}

void prepareInitialProgramIdentity(
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
    request.dependencies = storage.workspace.dependencies;
    request.dependency_capacity = storage.workspace.dependency_capacity;
    request.snapshot = &storage.workspace.snapshot;

    const auto playback = playback_preparer.prepare(pdta, request);
    assert(playback.ok());
    assert(playback.zone_count > 0);
    assert(playback.dependency_count > 0);
    assert(playback.unsupported_modulator_records == 0);
    assert(playback.preview_zone_count == 0);
    assert(residency_preparer.activateInitialProgram(
        channel,
        bank,
        program,
        token,
        storage.workspace.dependencies,
        playback.dependency_count));
    assert(playback_exchange.setInitialActive(&storage.workspace.snapshot));
}

struct SweepMetrics {
    std::size_t transitions = 0;
    std::size_t high_water_bytes = 0;
    std::size_t used_bytes = 0;
    std::uint64_t cache_evictions = 0;
    std::uint64_t cache_evicted_bytes = 0;
    std::uint64_t capacity_retries = 0;
};

SweepMetrics runFourMiBProgramSweep(
    std::uint8_t channel,
    std::uint16_t expected_bank,
    const char* label) {
    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    std::vector<std::uint32_t> arena_words(kFourMiB / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));

    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    std::array<WorkspaceStorage, 2> workspaces{};
    prepareInitialProgramIdentity(
        pdta,
        playback_preparer,
        residency_preparer,
        directory,
        playback_exchange,
        workspaces[0],
        channel,
        expected_bank,
        0,
        1000u + channel);

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

    std::size_t active_slot = 0;
    for (std::uint16_t program = 1; program < 128; ++program) {
        assert(router.apply(programChange(channel, static_cast<std::uint8_t>(program)), voices));
        assert(service.drainRealtimeRequests() == 1);
        const std::size_t ready_slot = 1u - active_slot;
        const auto prepared = service.serviceChannel(channel, workspaces[ready_slot].workspace);
        if (!prepared.ok()) {
            std::cerr
                << "GM128_STRESS_FAILURE label=" << label
                << " program=" << program
                << " service_status=" << static_cast<unsigned>(prepared.status)
                << " residency_error=" << static_cast<unsigned>(prepared.residency_error)
                << " playback_status=" << static_cast<unsigned>(prepared.playback_prepare.status)
                << '\n';
        }
        assert(prepared.ok());
        assert(prepared.request.bank == expected_bank);
        assert(prepared.request.program == program);
        assert(prepared.playback_prepare.unsupported_modulator_records == 0);
        assert(prepared.playback_prepare.preview_zone_count == 0);
        assert(coordinator.readyCompatible(channel));
        assert(committer.commitReadyAtBlockBoundary() == 1);
        const auto ack = service.acknowledgeCommitted(channel);
        assert(ack.ok());
        active_slot = ready_slot;
    }

    const auto service_stats = service.stats();
    assert(service_stats.service_successes == 127);
    assert(service_stats.playback_prepare_failures == 0);
    assert(service_stats.residency_prepare_failures == 0);
    assert(service_stats.publish_invariant_failures == 0);
    assert(service_stats.commit_acks == 127);

    const auto residency_stats = residency.stats();
    const auto prepare_stats = residency_preparer.stats();
    assert(residency_stats.capacity_bytes == kFourMiB);
    assert(residency_stats.used_bytes <= kFourMiB);
    assert(residency_stats.high_water_bytes <= kFourMiB);
    assert(residency_stats.stale_reference_faults == 0);
    assert(residency_stats.descriptor_conflicts == 0);
    assert(directory.stats().acquire_misses == 0);

    SweepMetrics metrics{};
    metrics.transitions = 127;
    metrics.high_water_bytes = residency_stats.high_water_bytes;
    metrics.used_bytes = residency_stats.used_bytes;
    metrics.cache_evictions = prepare_stats.cache_evictions;
    metrics.cache_evicted_bytes = prepare_stats.cache_evicted_bytes;
    metrics.capacity_retries = prepare_stats.capacity_retries;
    return metrics;
}

void testFourMiBFullGmProgramSweeps() {
    const SweepMetrics melodic = runFourMiBProgramSweep(0, 0, "melodic");
    const SweepMetrics drums = runFourMiBProgramSweep(
        kGmPercussionChannel, kSf2PercussionBank, "drums");

    assert(melodic.transitions == 127);
    assert(drums.transitions == 127);
    // At least one of the two complete 128-program sweeps must exercise the
    // safe-cache eviction path; otherwise this test is no longer creating the
    // intended 4 MiB pressure against the tracked full bank.
    assert(melodic.cache_evictions + drums.cache_evictions > 0);

    std::cout << "GM128_STRESS_4M_MELODIC_TRANSITIONS=" << melodic.transitions << '\n';
    std::cout << "GM128_STRESS_4M_MELODIC_HIGH_WATER=" << melodic.high_water_bytes << '\n';
    std::cout << "GM128_STRESS_4M_MELODIC_EVICTIONS=" << melodic.cache_evictions << '\n';
    std::cout << "GM128_STRESS_4M_MELODIC_EVICTED_BYTES=" << melodic.cache_evicted_bytes << '\n';
    std::cout << "GM128_STRESS_4M_MELODIC_CAPACITY_RETRIES=" << melodic.capacity_retries << '\n';
    std::cout << "GM128_STRESS_4M_DRUM_TRANSITIONS=" << drums.transitions << '\n';
    std::cout << "GM128_STRESS_4M_DRUM_HIGH_WATER=" << drums.high_water_bytes << '\n';
    std::cout << "GM128_STRESS_4M_DRUM_EVICTIONS=" << drums.cache_evictions << '\n';
    std::cout << "GM128_STRESS_4M_DRUM_EVICTED_BYTES=" << drums.cache_evicted_bytes << '\n';
    std::cout << "GM128_STRESS_4M_DRUM_CAPACITY_RETRIES=" << drums.capacity_retries << '\n';
}

void testFullBankSelectAndRapidLatestWinsOnFourMiB() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    std::vector<std::uint32_t> arena_words(kFourMiB / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);
    WorkspaceStorage active;
    WorkspaceStorage ready;
    prepareInitialProgramIdentity(
        pdta,
        playback_preparer,
        residency_preparer,
        directory,
        playback_exchange,
        active,
        0,
        0,
        0,
        4000);

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

    // Bank 1 is present in the tracked full bank. Exercise real CC0/CC32 state
    // into the Program Change request rather than calling the preparer directly.
    assert(router.apply(controlChange(0, 0, 0), voices));
    assert(router.apply(controlChange(0, 32, 1), voices));
    assert(router.apply(programChange(0, 0), voices));
    assert(service.drainRealtimeRequests() == 1);
    const auto bank_one = service.serviceChannel(0, ready.workspace);
    assert(bank_one.ok());
    assert(bank_one.request.bank == 1);
    assert(bank_one.request.program == 0);
    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(service.acknowledgeCommitted(0).ok());
    assert(playback_exchange.active(0)->bank == 1);

    // Latest-wins coalescing with the real full bank and the product-sized
    // arena. Only program 127 should reach the control-domain mailbox.
    assert(router.apply(controlChange(0, 32, 0), voices));
    for (std::uint8_t program = 0; program < 128; ++program) {
        assert(router.apply(programChange(0, program), voices));
    }
    assert(service.drainRealtimeRequests() == 1);
    gm_synth::gm::ProgramChangeRequest pending{};
    assert(mailbox.peek(0, &pending));
    assert(pending.bank == 0);
    assert(pending.program == 127);

    const auto latest = service.serviceChannel(0, active.workspace);
    assert(latest.ok());
    assert(latest.request.program == 127);
    assert(committer.commitReadyAtBlockBoundary() == 1);
    assert(service.acknowledgeCommitted(0).ok());
    assert(playback_exchange.active(0)->bank == 0);
    assert(playback_exchange.active(0)->program == 127);

    std::cout << "GM128_STRESS_BANK_SELECT_FULL_BANK=1\n";
    std::cout << "GM128_STRESS_RAPID_PROGRAM_CHANGE_COALESCED=128_TO_1\n";
}

void testMixedSixteenChannelAndVoiceStress() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    Sf2SampleSource sample_source;
    assert(pdta.begin(reader));
    assert(sample_source.begin(reader));

    std::vector<std::uint32_t> arena_words(kSixteenMiB / sizeof(std::uint32_t));
    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange residency_exchange;
    PlaybackProgramSnapshotExchange playback_exchange;
    assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    ProgramPrepareController residency_preparer(
        residency, directory, residency_exchange, &sample_source);
    Sf2PlaybackProgramPreparer playback_preparer(44100);

    std::array<WorkspaceStorage, 16> active_workspaces{};
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        const bool percussion = channel == kGmPercussionChannel;
        const std::uint16_t bank = percussion ? kSf2PercussionBank : 0;
        const std::uint8_t program = percussion ? 0 : static_cast<std::uint8_t>(channel * 8u);
        prepareInitialProgramIdentity(
            pdta,
            playback_preparer,
            residency_preparer,
            directory,
            playback_exchange,
            active_workspaces[channel],
            channel,
            bank,
            program,
            static_cast<std::uint32_t>(5000u + channel));
    }

    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramEventApplier events(playback_exchange);
    RealtimeProgramChangeRouter router(events, handoff);
    VoicePool voices(directory);

    // Representative 15 melodic programs plus the standard drum kit all sound
    // simultaneously. This is a functional host stress, not a physical timing
    // claim; the 16 MiB host arena deliberately isolates event/voice semantics.
    const auto starts_before_mix = events.stats().voices_started;
    const auto failures_before_mix = events.stats().voice_start_failures;
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        const std::uint8_t key = channel == kGmPercussionChannel ? 36 : 60;
        assert(router.apply(noteOn(channel, key, 100), voices));
    }
    assert(events.stats().voices_started >= starts_before_mix + 16);
    assert(events.stats().voice_start_failures == failures_before_mix);
    assert(voices.activeVoiceCount() >= 16);

    std::array<std::int32_t, 128> mix{};
    voices.renderAdd(mix.data(), mix.size() / 2u);

    // Exercise the complete live-controller set against actual full-bank
    // Voices while they are sounding.
    assert(router.apply(controlChange(0, 7, 90), voices));
    assert(router.apply(controlChange(0, 10, 32), voices));
    assert(router.apply(controlChange(0, 11, 100), voices));
    assert(router.apply(controlChange(0, 91, 80), voices));
    assert(router.apply(controlChange(0, 93, 70), voices));
    assert(router.apply(pitchBend(0, 16383), voices));
    assert(router.apply(controlChange(0, 101, 0), voices));
    assert(router.apply(controlChange(0, 100, 0), voices));
    assert(router.apply(controlChange(0, 6, 12), voices));
    assert(events.channelState(0).volume == 90);
    assert(events.channelState(0).pan == 32);
    assert(events.channelState(0).expression == 100);
    assert(events.channelState(0).reverb_send == 80);
    assert(events.channelState(0).chorus_send == 70);
    assert(events.channelState(0).pitch_bend_range_semitones == 12);

    voices.allSoundOff();
    assert(voices.activeVoiceCount() == 0);

    // Dense piano + sustain/release-tail pressure. Sixteen note instances is the
    // software-side guaranteed-voice target carried into final hardware timing.
    std::array<std::uint8_t, 16> piano_keys{};
    const auto failures_before_piano = events.stats().voice_start_failures;
    for (std::size_t i = 0; i < piano_keys.size(); ++i) {
        piano_keys[i] = static_cast<std::uint8_t>(36u + i * 4u);
        assert(router.apply(noteOn(0, piano_keys[i], 110), voices));
    }
    assert(events.stats().voice_start_failures == failures_before_piano);
    assert(voices.activeVoiceCount() >= 16);
    assert(router.apply(controlChange(0, 64, 127), voices));
    for (const std::uint8_t key : piano_keys) {
        assert(router.apply(noteOff(0, key), voices));
    }
    assert(voices.activeVoiceCount() > 0);
    assert(router.apply(controlChange(0, 64, 0), voices));
    voices.renderAdd(mix.data(), mix.size() / 2u);
    voices.allSoundOff();

    // Same-note retrigger pressure should create distinct logical starts and
    // remain bounded by the hard voice pool without sample-acquire faults.
    const auto starts_before_retrigger = events.stats().voices_started;
    const auto failures_before_retrigger = events.stats().voice_start_failures;
    for (std::size_t i = 0; i < 12; ++i) {
        assert(router.apply(noteOn(0, 60, 100), voices));
    }
    assert(events.stats().voices_started >= starts_before_retrigger + 12);
    assert(events.stats().voice_start_failures == failures_before_retrigger);
    assert(voices.activeVoiceCount() >= 12);
    assert(router.apply(noteOff(0, 60), voices));
    voices.renderAdd(mix.data(), mix.size() / 2u);
    voices.allSoundOff();

    // Standard GM drum burst. Exclusive-class instruments may choke older
    // siblings, so the gate counts successful starts rather than final active
    // slot count.
    const auto starts_before_drums = events.stats().voices_started;
    const auto failures_before_drums = events.stats().voice_start_failures;
    for (std::uint8_t key = 35; key < 51; ++key) {
        assert(router.apply(noteOn(kGmPercussionChannel, key, 115), voices));
    }
    assert(events.stats().voices_started >= starts_before_drums + 16);
    assert(events.stats().voice_start_failures == failures_before_drums);
    voices.renderAdd(mix.data(), mix.size() / 2u);

    const auto voice_stats = voices.stats();
    const auto publication_stats = directory.stats();
    const auto residency_stats = residency.stats();
    assert(voice_stats.peak_active_voices >= 16);
    assert(publication_stats.acquire_misses == 0);
    assert(publication_stats.stale_release_faults == 0);
    assert(residency_stats.stale_reference_faults == 0);
    assert(events.stats().missing_active_program == 0);
    assert(events.stats().percussion_program_unavailable == 0);

    std::cout << "GM128_STRESS_16CH_RESIDENT_BYTES=" << residency_stats.used_bytes << '\n';
    std::cout << "GM128_STRESS_16CH_HIGH_WATER=" << residency_stats.high_water_bytes << '\n';
    std::cout << "GM128_STRESS_VOICE_PEAK=" << voice_stats.peak_active_voices << '\n';
    std::cout << "GM128_STRESS_VOICE_STARTS=" << events.stats().voices_started << '\n';
    std::cout << "GM128_STRESS_VOICE_START_FAILURES=" << events.stats().voice_start_failures << '\n';
    std::cout << "GM128_STRESS_EXCLUSIVE_CHOKES=" << voice_stats.exclusive_choke_successes << '\n';
    std::cout << "GM128_STRESS_ACQUIRE_MISSES=" << publication_stats.acquire_misses << '\n';
}

}  // namespace

int main() {
    testFourMiBFullGmProgramSweeps();
    testFullBankSelectAndRapidLatestWinsOnFourMiB();
    testMixedSixteenChannelAndVoiceStress();
    std::cout << "Full FM GM128 stress qualification passed.\n";
    return 0;
}
