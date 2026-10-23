#include "PhysicalCleanSf2AudibleGate.h"

#include "I2sDmaQualification.h"
#include "QualificationIdleMonitor.h"
#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

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
#include <utility>

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
constexpr std::uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void logStepStart(const char* phase, std::int64_t gate_start_us) {
    const std::int64_t now = esp_timer_get_time();
    ESP_LOGI(
        kTag,
        "clean_sf2: STEP phase=%s state=START total_us=%lld",
        phase,
        static_cast<long long>(now - gate_start_us));
}

void logStepDone(
    const char* phase,
    bool ok,
    std::int64_t gate_start_us,
    std::int64_t step_start_us) {
    const std::int64_t now = esp_timer_get_time();
    ESP_LOGI(
        kTag,
        "clean_sf2: STEP phase=%s state=DONE ok=%u step_us=%lld total_us=%lld",
        phase,
        ok ? 1u : 0u,
        static_cast<long long>(now - step_start_us),
        static_cast<long long>(now - gate_start_us));
}

bool requireCleanIdleWindow(const char* phase) {
    if (verifiedQualificationIdleWindow(phase)) {
        return true;
    }
    ESP_LOGE(kTag, "clean_sf2: idle verification FAILED phase=%s", phase);
    return false;
}

struct SourceFillContext {
    sf2::Sf2SampleSource* source = nullptr;
    sample::SampleId sample_id = 0;
};

bool fillDependency(void* context, std::uint8_t* destination, std::size_t bytes) {
    auto* fill = static_cast<SourceFillContext*>(context);
    return fill != nullptr && fill->source != nullptr &&
        fill->source->fillSample(fill->sample_id, destination, bytes);
}

// Parsing/resolution scratch is control-domain only and can be large because
// Sf2SampleSource owns metadata for up to 512 samples. Keep it in PSRAM instead
// of consuming the ESP-IDF main-task stack. PlaybackZone storage stays alive for
// the whole clean-audio run because the active ProgramSnapshot points into it.
struct CleanPrepareWorkspace {
    sf2::Sf2SampleSource source{};
    std::array<sf2::NativeResolvedZone, kWorkspaceCapacity> native{};
    std::array<sf2::ResolvedZoneModulation, kWorkspaceCapacity> modulation{};
    std::array<gm::PlaybackZone, kWorkspaceCapacity> playback{};
    std::array<sample::SampleId, kWorkspaceCapacity> dependencies{};
};

struct CleanOwnership {
    std::array<sample::ResidentRecord, kWorkspaceCapacity> records{};
    std::array<bool, kWorkspaceCapacity> pinned{};
    std::array<bool, kWorkspaceCapacity> published{};
};

template <typename T, typename... Args>
T* allocateInternal(Args&&... args) {
    void* storage = heap_caps_malloc(sizeof(T), kInternalCaps);
    if (storage == nullptr) {
        return nullptr;
    }
    return new (storage) T(std::forward<Args>(args)...);
}

template <typename T>
void destroyInternal(T*& object) {
    if (object == nullptr) {
        return;
    }
    object->~T();
    heap_caps_free(object);
    object = nullptr;
}

// Keep objects that rely on S32C1I/internal-memory atomics and the production
// realtime Voice path in internal SRAM. The old aggregate CleanRuntime was
// 56,608 bytes in the d859b94 ELF, almost exactly the ~57 KiB largest free block
// measured on hardware. Allocate the same state as several bounded blocks so we
// do not require one fragile near-maximum contiguous allocation.
struct CleanRuntime {
    sample::PublishedSampleDirectory* directory = nullptr;
    voice::VoicePool* voices = nullptr;
    realtime::RealtimeEventQueue* queue = nullptr;
    realtime::BlockEventScheduler* scheduler = nullptr;
    gm::PlaybackProgramSnapshotExchange* programs = nullptr;
    gm::RealtimeProgramEventApplier* event_applier = nullptr;
    realtime::ScheduledAudioBlockRenderer* renderer = nullptr;
    CleanOwnership* ownership = nullptr;

    bool allocate() {
        directory = allocateInternal<sample::PublishedSampleDirectory>();
        if (directory == nullptr) {
            return false;
        }
        voices = allocateInternal<voice::VoicePool>(*directory);
        if (voices == nullptr) {
            return false;
        }
        queue = allocateInternal<realtime::RealtimeEventQueue>();
        if (queue == nullptr) {
            return false;
        }
        scheduler = allocateInternal<realtime::BlockEventScheduler>(*queue);
        if (scheduler == nullptr) {
            return false;
        }
        programs = allocateInternal<gm::PlaybackProgramSnapshotExchange>();
        if (programs == nullptr) {
            return false;
        }
        event_applier = allocateInternal<gm::RealtimeProgramEventApplier>(*programs);
        if (event_applier == nullptr) {
            return false;
        }
        renderer = allocateInternal<realtime::ScheduledAudioBlockRenderer>(
            *voices,
            *scheduler,
            &gm::RealtimeProgramEventApplier::callback,
            event_applier);
        if (renderer == nullptr) {
            return false;
        }
        ownership = allocateInternal<CleanOwnership>();
        return ownership != nullptr;
    }

    void release() {
        destroyInternal(ownership);
        destroyInternal(renderer);
        destroyInternal(event_applier);
        destroyInternal(programs);
        destroyInternal(scheduler);
        destroyInternal(queue);
        destroyInternal(voices);
        destroyInternal(directory);
    }
};

static_assert(sizeof(sample::PublishedSampleDirectory) <= 4u * 1024u);
static_assert(sizeof(voice::VoicePool) <= 32u * 1024u);
static_assert(sizeof(realtime::RealtimeEventQueue) <= 4u * 1024u);
static_assert(sizeof(realtime::BlockEventScheduler) <= 8u * 1024u);
static_assert(sizeof(gm::PlaybackProgramSnapshotExchange) <= 2u * 1024u);
static_assert(sizeof(gm::RealtimeProgramEventApplier) <= 24u * 1024u);
static_assert(sizeof(realtime::ScheduledAudioBlockRenderer) <= 16u * 1024u);
static_assert(sizeof(CleanOwnership) <= 4u * 1024u);

constexpr std::size_t kCleanRuntimeInternalBytes =
    sizeof(sample::PublishedSampleDirectory) +
    sizeof(voice::VoicePool) +
    sizeof(realtime::RealtimeEventQueue) +
    sizeof(realtime::BlockEventScheduler) +
    sizeof(gm::PlaybackProgramSnapshotExchange) +
    sizeof(gm::RealtimeProgramEventApplier) +
    sizeof(realtime::ScheduledAudioBlockRenderer) +
    sizeof(CleanOwnership);

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
    const std::int64_t gate_start_us = esp_timer_get_time();
    const char* path = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH;
    ESP_LOGI(
        kTag,
        "clean_sf2: START path=%s bank=%u program=%u channel=%u mode=real_sf2_dry",
        path,
        static_cast<unsigned>(kBank),
        static_cast<unsigned>(kProgram),
        static_cast<unsigned>(kChannel));

    storage::VfsRandomAccessReader reader;
    std::int64_t step_start_us = esp_timer_get_time();
    logStepStart("reader_open", gate_start_us);
    const bool reader_ok = reader.open(path);
    logStepDone("reader_open", reader_ok, gate_start_us, step_start_us);
    if (!reader_ok) {
        ESP_LOGE(kTag, "clean_sf2: cannot open %s", path);
        return false;
    }

    step_start_us = esp_timer_get_time();
    logStepStart("workspace_alloc", gate_start_us);
    void* workspace_storage = heap_caps_malloc(
        sizeof(CleanPrepareWorkspace),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool workspace_ok = workspace_storage != nullptr;
    logStepDone("workspace_alloc", workspace_ok, gate_start_us, step_start_us);
    if (workspace_storage == nullptr) {
        ESP_LOGE(
            kTag,
            "clean_sf2: PSRAM prepare-workspace allocation failed bytes=%u free_psram=%u largest_psram=%u",
            static_cast<unsigned>(sizeof(CleanPrepareWorkspace)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        return false;
    }
    auto* workspace = new (workspace_storage) CleanPrepareWorkspace{};
    const auto free_workspace = [&]() {
        workspace->~CleanPrepareWorkspace();
        heap_caps_free(workspace_storage);
    };

    if (!requireCleanIdleWindow("clean_before_parse")) {
        free_workspace();
        return false;
    }

    sf2::Sf2PdtaReader pdta;
    step_start_us = esp_timer_get_time();
    logStepStart("pdta_begin", gate_start_us);
    const bool pdta_ok = pdta.begin(reader);
    logStepDone("pdta_begin", pdta_ok, gate_start_us, step_start_us);
    if (!pdta_ok) {
        ESP_LOGE(kTag, "clean_sf2: pdta parse failed");
        free_workspace();
        return false;
    }
    if (!requireCleanIdleWindow("clean_after_pdta")) {
        free_workspace();
        return false;
    }

    step_start_us = esp_timer_get_time();
    logStepStart("sample_source_begin", gate_start_us);
    const bool source_ok = workspace->source.begin(reader);
    logStepDone("sample_source_begin", source_ok, gate_start_us, step_start_us);
    if (!source_ok) {
        ESP_LOGE(
            kTag,
            "clean_sf2: sample-source parse failed sample_error=%u",
            static_cast<unsigned>(workspace->source.lastError()));
        free_workspace();
        return false;
    }
    if (!requireCleanIdleWindow("clean_after_sample_source")) {
        free_workspace();
        return false;
    }

    gm::PlaybackProgramSnapshot snapshot{};

    sf2::PlaybackPrepareRequest request{};
    request.channel = kChannel;
    request.bank = kBank;
    request.program = kProgram;
    request.token = 1;
    request.bank_generation = 1;
    request.policy = sf2::PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = workspace->native.data();
    request.native_capacity = workspace->native.size();
    request.modulation_scratch = workspace->modulation.data();
    request.modulation_capacity = workspace->modulation.size();
    request.playback_zones = workspace->playback.data();
    request.playback_capacity = workspace->playback.size();
    request.dependencies = workspace->dependencies.data();
    request.dependency_capacity = workspace->dependencies.size();
    request.snapshot = &snapshot;

    step_start_us = esp_timer_get_time();
    logStepStart("program_prepare", gate_start_us);
    const sf2::PlaybackPrepareResult prepared =
        sf2::Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    const bool prepare_ok = prepared.ok() && snapshot.valid();
    logStepDone("program_prepare", prepare_ok, gate_start_us, step_start_us);
    if (!prepare_ok) {
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
        free_workspace();
        return false;
    }
    if (!requireCleanIdleWindow("clean_after_prepare")) {
        free_workspace();
        return false;
    }

    // Keep the first audible oracle deliberately dry. All other prepared SF2
    // behavior (sample region, tuning, loop, volume envelope, filter and native
    // modulation state) remains production-derived from the real Program 0.
    for (std::size_t zone_index = 0; zone_index < snapshot.zone_count; ++zone_index) {
        workspace->playback[zone_index].effects_send = gm::EffectsSendConfig{};
    }
    if (!snapshot.valid() || !snapshotMatchesAudibleKeys(snapshot)) {
        ESP_LOGE(
            kTag,
            "clean_sf2: Program 0 does not cover C4/E4/G4/C5 at velocity=%u",
            static_cast<unsigned>(kVelocity));
        free_workspace();
        return false;
    }

    CleanRuntime runtime{};
    step_start_us = esp_timer_get_time();
    logStepStart("runtime_alloc", gate_start_us);
    const bool runtime_alloc_ok = runtime.allocate();
    logStepDone("runtime_alloc", runtime_alloc_ok, gate_start_us, step_start_us);
    if (!runtime_alloc_ok) {
        ESP_LOGE(
            kTag,
            "clean_sf2: split internal runtime allocation failed total_bytes=%u free=%u largest=%u sizes[dir/voices/queue/scheduler/programs/applier/renderer/ownership]=%u/%u/%u/%u/%u/%u/%u/%u",
            static_cast<unsigned>(kCleanRuntimeInternalBytes),
            static_cast<unsigned>(heap_caps_get_free_size(kInternalCaps)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalCaps)),
            static_cast<unsigned>(sizeof(sample::PublishedSampleDirectory)),
            static_cast<unsigned>(sizeof(voice::VoicePool)),
            static_cast<unsigned>(sizeof(realtime::RealtimeEventQueue)),
            static_cast<unsigned>(sizeof(realtime::BlockEventScheduler)),
            static_cast<unsigned>(sizeof(gm::PlaybackProgramSnapshotExchange)),
            static_cast<unsigned>(sizeof(gm::RealtimeProgramEventApplier)),
            static_cast<unsigned>(sizeof(realtime::ScheduledAudioBlockRenderer)),
            static_cast<unsigned>(sizeof(CleanOwnership)));
        runtime.release();
        free_workspace();
        return false;
    }
    if (!requireCleanIdleWindow("clean_after_runtime_alloc")) {
        runtime.release();
        free_workspace();
        return false;
    }

    bool cleanup_ok = true;
    const auto cleanup = [&]() {
        runtime.voices->allSoundOff();
        for (std::size_t index = 0; index < prepared.dependency_count; ++index) {
            if (runtime.ownership->published[index]) {
                const sample::ResidentRecord* retired =
                    runtime.directory->unpublish(workspace->dependencies[index]);
                if (retired != &runtime.ownership->records[index] ||
                    !runtime.directory->canReclaim(&runtime.ownership->records[index])) {
                    cleanup_ok = false;
                }
            }
            if (runtime.ownership->pinned[index] &&
                !residency.unpin(workspace->dependencies[index])) {
                cleanup_ok = false;
            }
        }
        runtime.release();
        free_workspace();
    };

    bool setup_ok = runtime.directory->bankGeneration() == request.bank_generation;
    for (std::size_t index = 0; setup_ok && index < prepared.dependency_count; ++index) {
        const sample::SampleId sample_id = workspace->dependencies[index];
        sample::SampleDescriptor descriptor{};
        if (!workspace->source.descriptorFor(sample_id, &descriptor)) {
            ESP_LOGE(kTag, "clean_sf2: descriptor failed sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }

        const std::int64_t dependency_start_us = esp_timer_get_time();
        ESP_LOGI(
            kTag,
            "clean_sf2: DEPENDENCY state=START index=%u sample=%u pcm_bytes=%u total_us=%lld",
            static_cast<unsigned>(index),
            static_cast<unsigned>(sample_id),
            static_cast<unsigned>(descriptor.pcm_bytes),
            static_cast<long long>(dependency_start_us - gate_start_us));

        SourceFillContext fill{&workspace->source, sample_id};
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
        runtime.ownership->pinned[index] = true;

        const sample::ResidentSampleView view = residency.residentView(sample_id);
        if (!view.valid()) {
            ESP_LOGE(kTag, "clean_sf2: resident view invalid sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }

        runtime.ownership->records[index].bank_generation = runtime.directory->bankGeneration();
        runtime.ownership->records[index].sample_generation = view.generation;
        runtime.ownership->records[index].pcm = view.pcm;
        runtime.ownership->records[index].descriptor = view.descriptor;
        runtime.ownership->records[index].voice_refs.store(0, std::memory_order_relaxed);
        if (!runtime.directory->publish(sample_id, &runtime.ownership->records[index])) {
            ESP_LOGE(kTag, "clean_sf2: publish failed sample=%u",
                     static_cast<unsigned>(sample_id));
            setup_ok = false;
            break;
        }
        runtime.ownership->published[index] = true;

        const std::int64_t dependency_done_us = esp_timer_get_time();
        ESP_LOGI(
            kTag,
            "clean_sf2: DEPENDENCY state=DONE index=%u sample=%u elapsed_us=%lld total_us=%lld",
            static_cast<unsigned>(index),
            static_cast<unsigned>(sample_id),
            static_cast<long long>(dependency_done_us - dependency_start_us),
            static_cast<long long>(dependency_done_us - gate_start_us));
        if (!requireCleanIdleWindow("clean_dependency")) {
            setup_ok = false;
            break;
        }
    }

    if (setup_ok && !runtime.programs->setInitialActive(&snapshot)) {
        ESP_LOGE(kTag, "clean_sf2: active Program 0 publish failed");
        setup_ok = false;
    }

    constexpr std::uint64_t kStartFrame = 0;
    if (setup_ok && !enqueueCleanTimeline(*runtime.queue, kStartFrame)) {
        ESP_LOGE(kTag, "clean_sf2: event timeline enqueue failed");
        setup_ok = false;
    }

    if (!setup_ok) {
        cleanup();
        return false;
    }

    if (!requireCleanIdleWindow("clean_ready")) {
        cleanup();
        return false;
    }

    const auto queue_before = runtime.queue->stats();
    ESP_LOGI(
        kTag,
        "clean_sf2: READY zones=%u dependencies=%u events=%u cues=C4_2s,C_major_3s,C4_E4_G4_C5 dry_fx=1 prepare_psram_bytes=%u runtime_internal_total=%u internal_free=%u internal_largest=%u sizes[dir/voices/queue/scheduler/programs/applier/renderer/ownership]=%u/%u/%u/%u/%u/%u/%u/%u",
        static_cast<unsigned>(snapshot.zone_count),
        static_cast<unsigned>(prepared.dependency_count),
        static_cast<unsigned>(queue_before.accepted),
        static_cast<unsigned>(sizeof(CleanPrepareWorkspace)),
        static_cast<unsigned>(kCleanRuntimeInternalBytes),
        static_cast<unsigned>(heap_caps_get_free_size(kInternalCaps)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalCaps)),
        static_cast<unsigned>(sizeof(sample::PublishedSampleDirectory)),
        static_cast<unsigned>(sizeof(voice::VoicePool)),
        static_cast<unsigned>(sizeof(realtime::RealtimeEventQueue)),
        static_cast<unsigned>(sizeof(realtime::BlockEventScheduler)),
        static_cast<unsigned>(sizeof(gm::PlaybackProgramSnapshotExchange)),
        static_cast<unsigned>(sizeof(gm::RealtimeProgramEventApplier)),
        static_cast<unsigned>(sizeof(realtime::ScheduledAudioBlockRenderer)),
        static_cast<unsigned>(sizeof(CleanOwnership)));

    step_start_us = esp_timer_get_time();
    logStepStart("i2s_qualification", gate_start_us);
    const bool i2s_ok = runOptionalI2sDmaQualification(*runtime.renderer, kStartFrame);
    logStepDone("i2s_qualification", i2s_ok, gate_start_us, step_start_us);
    const auto renderer_stats = runtime.renderer->stats();
    const auto event_stats = runtime.event_applier->stats();
    const auto queue_after = runtime.queue->stats();

    runtime.voices->allSoundOff();
    const bool runtime_ok =
        queue_after.rejected_invalid == 0u &&
        queue_after.rejected_full == 0u &&
        queue_after.rejected_out_of_order == 0u &&
        runtime.queue->empty() &&
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
        "clean_sf2: RESULT i2s_ok=%u runtime_ok=%u events_applied=%llu voices_started=%llu missing_program=%llu no_match=%llu resolve_overflows=%llu voice_start_failures=%llu clipped_samples=%llu queue_remaining=%u total_us=%lld %s",
        i2s_ok ? 1u : 0u,
        runtime_ok ? 1u : 0u,
        static_cast<unsigned long long>(renderer_stats.events_applied),
        static_cast<unsigned long long>(event_stats.voices_started),
        static_cast<unsigned long long>(event_stats.missing_active_program),
        static_cast<unsigned long long>(event_stats.no_matching_zones),
        static_cast<unsigned long long>(event_stats.resolve_overflows),
        static_cast<unsigned long long>(event_stats.voice_start_failures),
        static_cast<unsigned long long>(renderer_stats.clipped_samples),
        static_cast<unsigned>(runtime.queue->size()),
        static_cast<long long>(esp_timer_get_time() - gate_start_us),
        (i2s_ok && runtime_ok) ? "PASS" : "FAIL");

    cleanup();
    if (!cleanup_ok) {
        ESP_LOGE(kTag, "clean_sf2: cleanup ownership invariant failed");
    }
    return i2s_ok && runtime_ok && cleanup_ok && qualificationIdleMonitorHealthy();
#endif
}

}  // namespace gm_synth::bringup
