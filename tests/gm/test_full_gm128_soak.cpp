#include "gm/ProgramChangeControlService.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "gm/ZoneResolver.h"
#include "sample/ProgramPrepareController.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/RandomAccessReader.h"
#include "voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::ProgramChangePrepareWorkspace;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::RealtimeProgramChangeRouter;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
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
using gm_synth::voice::LoopMode;
using gm_synth::voice::VoicePool;

constexpr const char* kFullBankPath =
    "assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2";
constexpr std::size_t kMiB = 1024u * 1024u;
constexpr std::size_t kArenaBytes = 4u * kMiB + kMiB / 2u;
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::size_t kBlockFrames = 64;
constexpr std::size_t kSoakBlocks = 22050;
constexpr std::uint32_t kReleaseExerciseMaxFrames = 4u * kSampleRate;
constexpr std::uint32_t kReleaseWarmupMaxFrames = kSampleRate;

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
        workspace.policy = PlaybackPreparePolicy::StrictQualified;
    }
};

struct NoteObservation {
    bool valid = false;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
    std::size_t layers = 0;
    std::uint32_t release_frames = 0;
    std::uint32_t warmup_frames = 0;
};

struct DiscoveryResult {
    NoteObservation max_layers{};
    NoteObservation release_tail{};
    std::uint32_t absolute_max_release_frames = 0;
    std::uint16_t max_zone_bank = 0;
    std::uint8_t max_zone_program = 0;
    std::size_t max_zone_count = 0;
};

RealtimeEvent noteOn(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity = 96) {
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

RealtimeEvent controlChange(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = channel;
    event.data1 = controller;
    event.data2 = value;
    return event;
}

RealtimeEvent pitchBend(std::uint8_t channel, std::uint16_t value) {
    RealtimeEvent event{};
    event.type = EventType::PitchBend;
    event.channel = channel;
    event.value = value;
    return event;
}

bool preparePlayback(
    Sf2PdtaReader& pdta,
    const Sf2PlaybackProgramPreparer& preparer,
    WorkspaceStorage& storage,
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token,
    std::uint32_t bank_generation,
    std::size_t* dependency_count = nullptr) {
    PlaybackPrepareRequest request{};
    request.channel = channel;
    request.bank = bank;
    request.program = program;
    request.token = token;
    request.bank_generation = bank_generation;
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
    if (!result.ok() || result.zone_count == 0 || result.dependency_count == 0 ||
        result.unsupported_modulator_records != 0 || result.preview_zone_count != 0 ||
        !storage.workspace.snapshot.valid()) {
        return false;
    }
    if (dependency_count != nullptr) {
        *dependency_count = result.dependency_count;
    }
    return true;
}

class RuntimeFixture {
public:
    explicit RuntimeFixture(std::size_t arena_bytes)
        : reader(kFullBankPath),
          arena_words(arena_bytes / sizeof(std::uint32_t)),
          residency_preparer(residency, directory, residency_exchange, &source),
          preparer(kSampleRate),
          events(playback_exchange),
          router(events, handoff),
          voices(directory) {
        assert(reader.valid());
        assert(pdta.begin(reader));
        assert(source.begin(reader));
        assert(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    }

    bool activate(
        WorkspaceStorage& storage,
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token) {
        std::size_t dependency_count = 0;
        if (!preparePlayback(
                pdta,
                preparer,
                storage,
                channel,
                bank,
                program,
                token,
                directory.bankGeneration(),
                &dependency_count)) {
            return false;
        }
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

    FileReader reader;
    Sf2PdtaReader pdta{};
    Sf2SampleSource source{};
    std::vector<std::uint32_t> arena_words;
    SampleResidencyManager residency{};
    PublishedSampleDirectory directory{};
    ProgramSnapshotExchange residency_exchange{};
    PlaybackProgramSnapshotExchange playback_exchange{};
    ProgramPrepareController residency_preparer;
    Sf2PlaybackProgramPreparer preparer;
    RealtimeProgramChangeHandoff handoff{};
    RealtimeProgramEventApplier events;
    RealtimeProgramChangeRouter router;
    VoicePool voices;
};

std::uint8_t channelForBank(std::uint16_t bank) {
    return bank == kSf2PercussionBank ? kGmPercussionChannel : 0;
}

DiscoveryResult discoverWorstPrograms() {
    FileReader reader(kFullBankPath);
    assert(reader.valid());
    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));
    Sf2PlaybackProgramPreparer preparer(kSampleRate);
    ZoneResolver resolver;
    MidiChannelState state{};
    state.resetAll();
    ResolvedVoiceBatch batch{};
    WorkspaceStorage storage;
    constexpr std::array<std::uint8_t, 5> kVelocities{1, 32, 64, 96, 127};
    DiscoveryResult discovered{};
    std::uint32_t token = 1000;

    for (const std::uint16_t bank : {std::uint16_t{0}, kSf2PercussionBank}) {
        const std::uint8_t channel = channelForBank(bank);
        for (std::uint16_t program = 0; program < 128; ++program) {
            assert(preparePlayback(
                pdta,
                preparer,
                storage,
                channel,
                bank,
                static_cast<std::uint8_t>(program),
                token++,
                1));

            const auto& snapshot = storage.workspace.snapshot;
            if (snapshot.zone_count > discovered.max_zone_count) {
                discovered.max_zone_count = snapshot.zone_count;
                discovered.max_zone_bank = bank;
                discovered.max_zone_program = static_cast<std::uint8_t>(program);
            }

            for (std::uint16_t key = 0; key < 128; ++key) {
                for (const std::uint8_t velocity : kVelocities) {
                    batch.clear();
                    const auto status = resolver.resolveNoteOn(
                        snapshot,
                        state,
                        channel,
                        static_cast<std::uint8_t>(key),
                        velocity,
                        1,
                        &batch);
                    if (status == ZoneResolveStatus::NoMatch) {
                        continue;
                    }
                    assert(status == ZoneResolveStatus::Ok);
                    assert(batch.count > 0);

                    if (!discovered.max_layers.valid ||
                        batch.count > discovered.max_layers.layers) {
                        discovered.max_layers = NoteObservation{
                            true,
                            bank,
                            static_cast<std::uint8_t>(program),
                            static_cast<std::uint8_t>(key),
                            velocity,
                            batch.count,
                            0,
                            0};
                    }

                    std::uint32_t batch_max_release = 0;
                    std::uint32_t batch_max_warmup = 0;
                    bool all_exclusive_zero = true;
                    bool has_continuous_loop = false;
                    for (std::size_t index = 0; index < batch.count; ++index) {
                        const auto& voice = batch.voices[index];
                        batch_max_release = std::max(
                            batch_max_release,
                            voice.envelope.release_frames);
                        const std::uint64_t warmup =
                            static_cast<std::uint64_t>(voice.envelope.delay_frames) +
                            voice.envelope.attack_frames + kBlockFrames;
                        batch_max_warmup = std::max<std::uint32_t>(
                            batch_max_warmup,
                            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                warmup,
                                std::numeric_limits<std::uint32_t>::max())));
                        discovered.absolute_max_release_frames = std::max(
                            discovered.absolute_max_release_frames,
                            voice.envelope.release_frames);
                        all_exclusive_zero = all_exclusive_zero && voice.exclusive_class == 0;
                        has_continuous_loop =
                            has_continuous_loop || voice.loop_mode == LoopMode::Continuous;
                    }

                    if (bank != 0 || !all_exclusive_zero || !has_continuous_loop ||
                        batch.count > VoicePool::kHardVoiceCap / 2u ||
                        batch_max_release < kBlockFrames ||
                        batch_max_release > kReleaseExerciseMaxFrames ||
                        batch_max_warmup > kReleaseWarmupMaxFrames) {
                        continue;
                    }

                    const NoteObservation candidate{
                        true,
                        bank,
                        static_cast<std::uint8_t>(program),
                        static_cast<std::uint8_t>(key),
                        velocity,
                        batch.count,
                        batch_max_release,
                        batch_max_warmup};
                    if (!discovered.release_tail.valid ||
                        candidate.release_frames > discovered.release_tail.release_frames) {
                        discovered.release_tail = candidate;
                    }
                }
            }
        }
    }

    assert(discovered.max_layers.valid);
    assert(discovered.release_tail.valid);
    assert(discovered.max_zone_count > 0);
    return discovered;
}

void renderFrames(VoicePool& voices, std::size_t frames) {
    std::array<std::int32_t, kBlockFrames * 2u> mix{};
    std::size_t rendered = 0;
    while (rendered < frames && voices.activeVoiceCount() != 0) {
        const std::size_t count = std::min<std::size_t>(kBlockFrames, frames - rendered);
        mix.fill(0);
        voices.renderAdd(mix.data(), count);
        rendered += count;
    }
}

void testWorstLayeredNote(const DiscoveryResult& discovered) {
    RuntimeFixture f(kArenaBytes);
    WorkspaceStorage workspace;
    const std::uint8_t channel = channelForBank(discovered.max_layers.bank);
    assert(f.activate(
        workspace,
        channel,
        discovered.max_layers.bank,
        discovered.max_layers.program,
        5000));

    const auto resolved_before = f.events.stats().voices_resolved;
    const auto started_before = f.events.stats().voices_started;
    const auto failures_before = f.events.stats().voice_start_failures;
    assert(f.router.apply(
        noteOn(channel, discovered.max_layers.key, discovered.max_layers.velocity),
        f.voices));
    assert(f.events.stats().voices_resolved - resolved_before == discovered.max_layers.layers);
    assert(f.events.stats().voices_started - started_before == discovered.max_layers.layers);
    assert(f.events.stats().voice_start_failures == failures_before);
    assert(f.voices.activeVoiceCount() == discovered.max_layers.layers);
    renderFrames(f.voices, kBlockFrames);
    f.voices.allSoundOff();
    assert(f.directory.stats().live_voice_refs == 0);

    std::cout << "GM128_SOAK_MAX_LAYERS=" << discovered.max_layers.layers << '\n';
    std::cout << "GM128_SOAK_MAX_LAYER_BANK=" << discovered.max_layers.bank << '\n';
    std::cout << "GM128_SOAK_MAX_LAYER_PROGRAM="
              << static_cast<unsigned>(discovered.max_layers.program) << '\n';
    std::cout << "GM128_SOAK_MAX_LAYER_KEY="
              << static_cast<unsigned>(discovered.max_layers.key) << '\n';
    std::cout << "GM128_SOAK_MAX_LAYER_VELOCITY="
              << static_cast<unsigned>(discovered.max_layers.velocity) << '\n';
}

void testWorstZoneHeavyProgram(const DiscoveryResult& discovered) {
    RuntimeFixture f(kArenaBytes);
    WorkspaceStorage workspace;
    const std::uint8_t channel = channelForBank(discovered.max_zone_bank);
    assert(f.activate(
        workspace,
        channel,
        discovered.max_zone_bank,
        discovered.max_zone_program,
        6000));
    assert(workspace.workspace.snapshot.zone_count == discovered.max_zone_count);

    ZoneResolver resolver;
    MidiChannelState state{};
    state.resetAll();
    ResolvedVoiceBatch batch{};
    std::size_t playable_keys = 0;
    for (std::uint16_t key = 0; key < 128; ++key) {
        batch.clear();
        const auto status = resolver.resolveNoteOn(
            workspace.workspace.snapshot,
            state,
            channel,
            static_cast<std::uint8_t>(key),
            96,
            1,
            &batch);
        if (status == ZoneResolveStatus::NoMatch) {
            continue;
        }
        assert(status == ZoneResolveStatus::Ok);
        assert(f.router.apply(noteOn(channel, static_cast<std::uint8_t>(key), 96), f.voices));
        ++playable_keys;
        renderFrames(f.voices, kBlockFrames);
        f.voices.allSoundOff();
    }

    assert(playable_keys > 0);
    assert(f.events.stats().voice_start_failures == 0);
    assert(f.directory.stats().acquire_misses == 0);
    assert(f.directory.stats().live_voice_refs == 0);
    std::cout << "GM128_SOAK_MAX_ZONE_BANK=" << discovered.max_zone_bank << '\n';
    std::cout << "GM128_SOAK_MAX_ZONE_PROGRAM="
              << static_cast<unsigned>(discovered.max_zone_program) << '\n';
    std::cout << "GM128_SOAK_MAX_ZONE_COUNT=" << discovered.max_zone_count << '\n';
    std::cout << "GM128_SOAK_MAX_ZONE_PLAYABLE_KEYS=" << playable_keys << '\n';
}

void testReleaseTailAccumulation(const DiscoveryResult& discovered) {
    RuntimeFixture f(kArenaBytes);
    WorkspaceStorage workspace;
    assert(f.activate(
        workspace,
        0,
        0,
        discovered.release_tail.program,
        7000));

    const std::size_t layers = discovered.release_tail.layers;
    assert(layers > 0 && layers <= VoicePool::kHardVoiceCap / 2u);
    const std::size_t instances = std::min<std::size_t>(
        8,
        VoicePool::kHardVoiceCap / layers);
    assert(instances >= 2);

    const auto failures_before = f.events.stats().voice_start_failures;
    for (std::size_t i = 0; i < instances; ++i) {
        assert(f.router.apply(
            noteOn(0, discovered.release_tail.key, discovered.release_tail.velocity),
            f.voices));
    }
    assert(f.events.stats().voice_start_failures == failures_before);
    assert(f.voices.activeVoiceCount() >= instances);
    const std::size_t release_start_voices = f.voices.activeVoiceCount();

    renderFrames(f.voices, discovered.release_tail.warmup_frames);
    assert(f.voices.activeVoiceCount() > 0);
    assert(f.directory.stats().live_voice_refs > 0);
    assert(f.router.apply(noteOff(0, discovered.release_tail.key), f.voices));
    assert(f.voices.activeVoiceCount() > 0);

    std::size_t rendered_release_frames = 0;
    std::array<std::int32_t, kBlockFrames * 2u> mix{};
    const std::size_t release_limit =
        static_cast<std::size_t>(discovered.release_tail.release_frames) +
        4u * kBlockFrames;
    while (f.voices.activeVoiceCount() != 0 && rendered_release_frames <= release_limit) {
        mix.fill(0);
        f.voices.renderAdd(mix.data(), kBlockFrames);
        rendered_release_frames += kBlockFrames;
    }
    assert(f.voices.activeVoiceCount() == 0);
    assert(f.directory.stats().live_voice_refs == 0);
    assert(f.directory.stats().stale_release_faults == 0);

    std::cout << "GM128_SOAK_RELEASE_PROGRAM="
              << static_cast<unsigned>(discovered.release_tail.program) << '\n';
    std::cout << "GM128_SOAK_RELEASE_KEY="
              << static_cast<unsigned>(discovered.release_tail.key) << '\n';
    std::cout << "GM128_SOAK_RELEASE_VELOCITY="
              << static_cast<unsigned>(discovered.release_tail.velocity) << '\n';
    std::cout << "GM128_SOAK_RELEASE_WARMUP_FRAMES="
              << discovered.release_tail.warmup_frames << '\n';
    std::cout << "GM128_SOAK_RELEASE_CONFIG_FRAMES="
              << discovered.release_tail.release_frames << '\n';
    std::cout << "GM128_SOAK_RELEASE_RENDERED_FRAMES="
              << rendered_release_frames << '\n';
    std::cout << "GM128_SOAK_RELEASE_START_VOICES=" << release_start_voices << '\n';
    std::cout << "GM128_SOAK_ABSOLUTE_MAX_RELEASE_FRAMES="
              << discovered.absolute_max_release_frames << '\n';
}

std::uint8_t baselineProgram(std::uint8_t channel) {
    return channel == kGmPercussionChannel
        ? 0
        : static_cast<std::uint8_t>(channel * 8u);
}

std::uint16_t baselineBank(std::uint8_t channel) {
    return channel == kGmPercussionChannel ? kSf2PercussionBank : 0;
}

void testThirtyTwoSecondMixedSoak() {
    RuntimeFixture f(kArenaBytes);
    std::array<WorkspaceStorage, 16> workspaces{};
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        assert(f.activate(
            workspaces[channel],
            channel,
            baselineBank(channel),
            baselineProgram(channel),
            static_cast<std::uint32_t>(8000u + channel)));
    }

    std::array<std::int32_t, kBlockFrames * 2u> mix{};
    std::array<std::uint8_t, 16> current_keys{};
    std::uint64_t checksum = 0;

    for (std::size_t block = 0; block < kSoakBlocks; ++block) {
        const std::size_t phase = block % 256u;
        const std::size_t epoch = block / 256u;

        if (phase == 0) {
            f.voices.allSoundOff();
            assert(f.router.apply(
                controlChange(0, 64, (epoch & 1u) == 0u ? 127 : 0),
                f.voices));
            for (std::uint8_t channel = 0; channel < 16; ++channel) {
                const std::uint8_t key = channel == kGmPercussionChannel ? 36 : 60;
                current_keys[channel] = key;
                assert(f.router.apply(noteOn(channel, key, 100), f.voices));
            }
        } else if (phase == 64) {
            for (std::uint8_t channel = 0; channel < 16; ++channel) {
                assert(f.router.apply(noteOff(channel, current_keys[channel]), f.voices));
            }
        } else if (phase == 128) {
            assert(f.router.apply(controlChange(0, 64, 0), f.voices));
        } else if (phase == 160) {
            const std::uint8_t channel = static_cast<std::uint8_t>(epoch % 16u);
            assert(f.router.apply(
                controlChange(
                    channel,
                    7,
                    static_cast<std::uint8_t>(80u + epoch % 48u)),
                f.voices));
            assert(f.router.apply(
                controlChange(
                    channel,
                    10,
                    static_cast<std::uint8_t>((epoch * 11u) % 128u)),
                f.voices));
            assert(f.router.apply(
                controlChange(
                    channel,
                    11,
                    static_cast<std::uint8_t>(96u + epoch % 32u)),
                f.voices));
            assert(f.router.apply(
                controlChange(channel, 91, static_cast<std::uint8_t>(epoch % 128u)),
                f.voices));
            assert(f.router.apply(
                controlChange(
                    channel,
                    93,
                    static_cast<std::uint8_t>((epoch * 3u) % 128u)),
                f.voices));
            assert(f.router.apply(
                pitchBend(
                    channel,
                    static_cast<std::uint16_t>((epoch * 257u) % 16384u)),
                f.voices));
        } else if (phase == 192) {
            f.voices.allSoundOff();
            for (std::uint8_t key = 35; key < 43; ++key) {
                assert(f.router.apply(
                    noteOn(kGmPercussionChannel, key, 115),
                    f.voices));
            }
        }

        mix.fill(0);
        f.voices.renderAdd(mix.data(), kBlockFrames);
        checksum ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(mix.front())) +
            0x9e3779b97f4a7c15ULL + (checksum << 6u) + (checksum >> 2u);
        checksum ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(mix.back())) +
            0x517cc1b727220a95ULL + (checksum << 7u) + (checksum >> 3u);
    }

    assert(f.router.apply(controlChange(0, 64, 0), f.voices));
    f.voices.allSoundOff();
    const auto event_stats = f.events.stats();
    const auto voice_stats = f.voices.stats();
    const auto publication_stats = f.directory.stats();
    const auto residency_stats = f.residency.stats();

    assert(f.voices.activeVoiceCount() == 0);
    assert(event_stats.voice_start_failures == 0);
    assert(event_stats.missing_active_program == 0);
    assert(event_stats.percussion_program_unavailable == 0);
    assert(publication_stats.acquire_misses == 0);
    assert(publication_stats.stale_release_faults == 0);
    assert(publication_stats.live_voice_refs == 0);
    assert(residency_stats.stale_reference_faults == 0);
    assert(residency_stats.descriptor_conflicts == 0);
    assert(residency_stats.high_water_bytes <= kArenaBytes);
    assert(voice_stats.rendered_voice_frames > 0);
    assert(voice_stats.peak_active_voices >= 16);
    assert(voice_stats.start_failures_full == 0);
    assert(voice_stats.start_failures_admission == 0);
    assert(checksum != 0);

    const std::size_t rendered_frames = kSoakBlocks * kBlockFrames;
    const std::size_t duration_ms = rendered_frames * 1000u / kSampleRate;
    std::cout << "GM128_SOAK_BLOCKS=" << kSoakBlocks << '\n';
    std::cout << "GM128_SOAK_FRAMES=" << rendered_frames << '\n';
    std::cout << "GM128_SOAK_DURATION_MS=" << duration_ms << '\n';
    std::cout << "GM128_SOAK_ARENA_BYTES=" << kArenaBytes << '\n';
    std::cout << "GM128_SOAK_ARENA_HIGH_WATER=" << residency_stats.high_water_bytes << '\n';
    std::cout << "GM128_SOAK_VOICE_PEAK=" << voice_stats.peak_active_voices << '\n';
    std::cout << "GM128_SOAK_VOICE_STARTS=" << event_stats.voices_started << '\n';
    std::cout << "GM128_SOAK_VOICES_FINISHED=" << voice_stats.voices_finished << '\n';
    std::cout << "GM128_SOAK_RENDERED_VOICE_FRAMES="
              << voice_stats.rendered_voice_frames << '\n';
    std::cout << "GM128_SOAK_ACQUIRE_MISSES=" << publication_stats.acquire_misses << '\n';
    std::cout << "GM128_SOAK_FINAL_LIVE_REFS=" << publication_stats.live_voice_refs << '\n';
    std::cout << "GM128_SOAK_CHECKSUM=" << checksum << '\n';
}

}  // namespace

int main() {
    const DiscoveryResult discovered = discoverWorstPrograms();
    testWorstLayeredNote(discovered);
    testWorstZoneHeavyProgram(discovered);
    testReleaseTailAccumulation(discovered);
    testThirtyTwoSecondMixedSoak();
    std::cout << "Full FM GM128 soak qualification passed.\n";
    return 0;
}
