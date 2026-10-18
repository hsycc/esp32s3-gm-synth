#include "PhysicalCleanSf2AudibleGate.h"

#include "I2sDmaQualification.h"
#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "realtime/BlockEventScheduler.h"
#include "realtime/RealtimeEventQueue.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/VfsRandomAccessReader.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr std::uint8_t kChannel = 0;
constexpr std::uint16_t kBank = 0;
constexpr std::uint8_t kProgram = 0;
constexpr std::uint8_t kVelocity = 96;
constexpr std::size_t kWorkspaceCapacity = 8;
constexpr std::uint64_t kFramesPerSecond = 44100;
constexpr std::uint64_t kHalfSecondFrames = kFramesPerSecond / 2u;
constexpr std::uint64_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;

struct SourceFillContext {
    sf2::Sf2SampleSource* source = nullptr;
    sample::SampleId sample_id = 0;
};

bool fillDependency(void* context, std::uint8_t* destination, std::size_t bytes) {
    auto* fill = static_cast<SourceFillContext*>(context);
    return fill != nullptr && fill->source != nullptr &&
        fill->source->fillSample(fill->sample_id, destination, bytes);
}

struct CleanRuntime {
    sample::PublishedSampleDirectory directory{};
    voice::VoicePool voices;
    realtime::RealtimeEventQueue queue{};
    realtime::BlockEventScheduler scheduler;
    gm::PlaybackProgramSnapshotExchange programs{};
    gm::RealtimeProgramEventApplier event_applier;
    realtime::ScheduledAudioBlockRenderer renderer;

    CleanRuntime()
        : voices(directory),
          scheduler(queue),
          event_applier(programs),
          renderer(
              voices,
              scheduler,
              &gm::RealtimeProgramEventApplier::callback,
              &event_applier) {}
};

bool snapshotMatchesAudibleKeys(const gm::PlaybackProgramSnapshot& snapshot) {
    constexpr std::array<std::uint8_t, 4> kKeys{60, 64, 67, 72};
    for (const std::uint8_t key : kKeys) {
        bool matched = false;
        for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
            if (snapshot.zones[zone_index].matches(key, kVelocity)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

bool pushNote(
    realtime::RealtimeEventQueue& queue,
    std::uint64_t frame,
    realtime::EventType type,
    std::uint8_t key,
    std::uint8_t velocity = 0) {
    realtime::RealtimeEvent event{};
    event.timestamp_frame = frame;
    event.type = type;
    event.channel = kChannel;
    event.data1 = key;
    event.data2 = velocity;
    return queue.tryPush(event);
}

bool enqueueCleanTimeline(realtime::RealtimeEventQueue& queue, std::uint64_t start_frame) {
    // The I2S qualifier consumes warm-up blocks before its measured phase. Keep
    // them silent, then play a sparse sequence that is easy to judge by ear:
    //   C4 single note -> silence -> C4/E4/G4 chord -> silence -> C4/E4/G4/C5.
    const std::uint64_t warmup_frames =
        static_cast<std::uint64_t>(CONFIG_GM_SYNTH_I2S_QUAL_WARMUP_BLOCKS) *
        kFramesPerBlock;
    std::uint64_t cursor = start_frame + warmup_frames + (2u * kFramesPerBlock);

    if (!pushNote(queue, cursor, realtime::EventType::NoteOn, 60, kVelocity) ||
        !pushNote(queue, cursor + 2u * kFramesPerSecond,
                  realtime::EventType::NoteOff, 60)) {
        return false;
    }

    cursor += 2u * kFramesPerSecond + kHalfSecondFrames;
    for (const std::uint8_t key : {std::uint8_t{60}, std::uint8_t{64}, std::uint8_t{67}}) {
        if (!pushNote(queue, cursor, realtime::EventType::NoteOn, key, kVelocity)) {
            return false;
        }
    }
    const std::uint64_t chord_off = cursor + 3u * kFramesPerSecond;
    for (const std::uint8_t key : {std::uint8_t{60}, std::uint8_t{64}, std::uint8_t{67}}) {
        if (!pushNote(queue, chord_off, realtime::EventType::NoteOff, key)) {
            return false;
        }
    }

    cursor = chord_off + kHalfSecondFrames;
    constexpr std::array<std::uint8_t, 4> kScaleKeys{60, 64, 67, 72};
    for (const std::uint8_t key : kScaleKeys) {
        if (!pushNote(queue, cursor, realtime::EventType::NoteOn, key, kVelocity) ||
            !pushNote(queue, cursor + kFramesPerSecond,
                      realtime::EventType::NoteOff, key)) {
            return false;
        }
        cursor += kFramesPerSecond + (kFramesPerSecond / 4u);
    }
    return true;
}

}  // namespace

bool runPhysicalCleanSf2AudibleGate(sample::SampleResidencyManager& residency) {
#if !defined(CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE)
    ESP_LOGE(kTag, "clean_sf2: storage fixture path is unavailable in this build");
    return false;
#else
    const char* path = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH;
    ESP_LOGI(
        kTag,
        "clean_sf2: START path=%s bank=%u program=%u channel=%u mode=real_sf2_dry",
        path,
        static_cast<unsigned>(kBank),
        static_cast<unsigned>(kProgram),
        static_cast<unsigned>(kChannel));

    storage::VfsRandomAccessReader reader;
    if (!reader.open(path)) {
        ESP_LOGE(kTag, "clean_sf2: cannot open %s", path);
        return false;
    }

    sf2::Sf2PdtaReader pdta;
    sf2::Sf2SampleSource source;
    if (!pdta.begin(reader) || !source.begin(reader)) {
        ESP_LOGE(
            kTag,
            "clean_sf2: real SF2 parse failed sample_error=%u",
            static_cast<unsigned>(source.lastError()));
        return false;
    }

    std::array<sf2::NativeResolvedZone, kWorkspaceCapacity> native{};
    std::array<sf2::ResolvedZoneModulation, kWorkspaceCapacity> modulation{};
    std::array<gm::PlaybackZone, kWorkspaceCapacity> playback{};
    std::array<sample::SampleId, kWorkspaceCapacity> dependencies{};
    gm::PlaybackProgramSnapshot snapshot{};

    sf2::PlaybackPrepareRequest request{};
    request.channel = kChannel;
    request.bank = kBank;
    request.program = kProgram;
    request.token = 1;
    request.bank_generation = 1;
    request.policy = sf2::PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = native.data();
    request.native_capacity = native.size();
    request.modulation_scratch = modulation.data();
    request.modulation_capacity = modulation.size();
    request.playback_zones = playback.data();
    request.playback_capacity = playback.size();
    request.dependencies = dependencies.data();
    request.dependency_capacity = dependencies.size();
    request.snapshot = &snapshot;

    const sf2::PlaybackPrepareResult prepared =
        sf2::Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    if (!prepared.ok() || !snapshot.valid()) {
        ESP_LOGE(
            kTag,
            "clean_sf2: program prepare failed status=%u resolve=%u mod=%u zones=%u required_zones=%u dependencies=%u required_dependencies=%u issues=0x%llx",
            static_cast<unsigned>(prepared.status),
            static_cast<unsigned>(prepared.resolve_status),
            static_cast<unsigned>(prepared.modulator_resolve_status),
            static_cast<unsigned>(prepared.zone_count),
            static_cast<unsigned>(prepared.required_zone_capacity),
            static_cast<unsigned>(prepared.dependency_count),
            static_cast<unsigned>(prepared.required_dependency_capacity),
            static_cast<unsigned long long>(prepared.issue_union));
        return false;
    }

    // Keep the first audible oracle deliberately dry. All other prepared SF2
    // behavior (sample region, tuning, loop, volume envelope, filter and native
    // modulation state) remains production-derived from the real Program 0.
    for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
        playback[zone_index].effects_send = gm::EffectsSendConfig{};
    }
    if (!snapshot.valid() || !snapshotMatchesAudibleKeys(snapshot)) {
        ESP_LOGE(
            kTag,
            "clean_sf2: Program 0 does not cover C4/E4/G4/C5 at velocity=%u",
            static_cast<unsigned>(kVelocity));
        return false;
    }

    void* runtime_storage = heap_caps_malloc(
        sizeof(CleanRuntime),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (runtime_storage == nullptr) {
        ESP_LOGE(
            kTag,
            "clean_sf2: internal runtime allocation failed bytes=%u free=%u largest=%u",
            static_cast<unsigned>(sizeof(CleanRuntime)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }
    auto* runtime = new (runtime_storage) CleanRuntime{};

    std::array<sample::ResidentRecord, kWorkspaceCapacity> records{};
    std::array<bool, kWorkspaceCapacity> pinned{};
    std::array<bool, kWorkspaceCapacity> published{};
    bool cleanup_ok = true;

    const auto cleanup = [&]() {
        runtime->voices.allSoundOff();
        for (std::size_t index = 0; index < prepared.dependency_count; ++index) {
            if (published[index]) {
                const sample::ResidentRecord* retired =
                    runtime->directory.unpublish(dependencies[index]);
                if (retired != &records[index] || !runtime->directory.canReclaim(&records[index])) {
                    cleanup_ok = false;
                }
            }
            if (pinned[index] && !residency.unpin(dependencies[index])) {
                cleanup_ok = false;
            }
        }
        runtime->~CleanRuntime();
        heap_caps_free(runtime_storage);
    };

    bool setup_ok = runtime->directory.bankGeneration() == request.bank_generation;
    for (std::size_t index = 0; setup_ok && index < prepared.dependency_count; ++index) {
        const sample::SampleId sample_id = dependencies[index];
        sample::SampleDescriptor descriptor{};
        if (!source.descriptorFor(sample_id, &descriptor)) {
            ESP_LOGE(kTag, "clean_sf2: descriptor failed sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }

        SourceFillContext fill{&source, sample_id};
        if (!residency.loadFromSource(descriptor, &fillDependency, &fill)) {
            ESP_LOGE(
                kTag,
                "clean_sf2: residency load/reuse failed sample=%u error=%u",
                static_cast<unsigned>(sample_id),
                static_cast<unsigned>(residency.lastError()));
            setup_ok = false;
            break;
        }
        if (!residency.pin(sample_id)) {
            ESP_LOGE(kTag, "clean_sf2: pin failed sample=%u error=%u",
                     static_cast<unsigned>(sample_id),
                     static_cast<unsigned>(residency.lastError()));
            setup_ok = false;
            break;
        }
        pinned[index] = true;

        const sample::ResidentSampleView view = residency.residentView(sample_id);
        if (!view.valid()) {
            ESP_LOGE(kTag, "clean_sf2: resident view invalid sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }

        records[index].bank_generation = runtime->directory.bankGeneration();
        records[index].sample_generation = view.generation;
        records[index].pcm = view.pcm;
        records[index].descriptor = view.descriptor;
        records[index].voice_refs.store(0, std::memory_order_relaxed);
        if (!runtime->directory.publish(sample_id, &records[index])) {
            ESP_LOGE(kTag, "clean_sf2: publish failed sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }
        published[index] = true;
    }

    if (setup_ok && !runtime->programs.setInitialActive(&snapshot)) {
        ESP_LOGE(kTag, "clean_sf2: active Program 0 publish failed");
        setup_ok = false;
    }

    constexpr std::uint64_t kStartFrame = 0;
    if (setup_ok && !enqueueCleanTimeline(runtime->queue, kStartFrame)) {
        ESP_LOGE(kTag, "clean_sf2: event timeline enqueue failed");
        setup_ok = false;
    }

    if (!setup_ok) {
        cleanup();
        return false;
    }

    const auto queue_before = runtime->queue.stats();
    ESP_LOGI(
        kTag,
        "clean_sf2: READY zones=%u dependencies=%u events=%u cues=C4_2s,C_major_3s,C4_E4_G4_C5 dry_fx=1 runtime_internal_bytes=%u",
        static_cast<unsigned>(snapshot.zone_count),
        static_cast<unsigned>(prepared.dependency_count),
        static_cast<unsigned>(queue_before.accepted),
        static_cast<unsigned>(sizeof(CleanRuntime)));

    const bool i2s_ok = runOptionalI2sDmaQualification(runtime->renderer, kStartFrame);
    const auto renderer_stats = runtime->renderer.stats();
    const auto event_stats = runtime->event_applier.stats();
    const auto queue_after = runtime->queue.stats();

    runtime->voices.allSoundOff();
    const bool runtime_ok =
        queue_after.rejected_invalid == 0u &&
        queue_after.rejected_full == 0u &&
        queue_after.rejected_out_of_order == 0u &&
        runtime->queue.empty() &&
        renderer_stats.events_rejected == 0u &&
        renderer_stats.events_applied == queue_before.accepted &&
        event_stats.missing_active_program == 0u &&
        event_stats.no_matching_zones == 0u &&
        event_stats.resolve_overflows == 0u &&
        event_stats.voice_start_failures == 0u &&
        event_stats.voices_started > 0u &&
        renderer_stats.clipped_samples == 0u;

    ESP_LOGI(
        kTag,
        "clean_sf2: RESULT i2s_ok=%u runtime_ok=%u events_applied=%llu voices_started=%llu missing_program=%llu no_match=%llu resolve_overflows=%llu voice_start_failures=%llu clipped_samples=%llu queue_remaining=%u %s",
        i2s_ok ? 1u : 0u,
        runtime_ok ? 1u : 0u,
        static_cast<unsigned long long>(renderer_stats.events_applied),
        static_cast<unsigned long long>(event_stats.voices_started),
        static_cast<unsigned long long>(event_stats.missing_active_program),
        static_cast<unsigned long long>(event_stats.no_matching_zones),
        static_cast<unsigned long long>(event_stats.resolve_overflows),
        static_cast<unsigned long long>(event_stats.voice_start_failures),
        static_cast<unsigned long long>(renderer_stats.clipped_samples),
        static_cast<unsigned>(runtime->queue.size()),
        (i2s_ok && runtime_ok) ? "PASS" : "FAIL");

    cleanup();
    if (!cleanup_ok) {
        ESP_LOGE(kTag, "clean_sf2: cleanup ownership invariant failed");
    }
    return i2s_ok && runtime_ok && cleanup_ok;
#endif
}

}  // namespace gm_synth::bringup
