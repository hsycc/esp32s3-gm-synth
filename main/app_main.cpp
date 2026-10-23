#include "sdkconfig.h"

#include "DspStageTimingBenchmark.h"
#include "EffectsTimingBenchmark.h"
#include "ProgramPrepareBenchmark.h"
#include "QualificationIdleMonitor.h"
#include "RenderTimingBenchmark.h"
#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
#include "PhysicalCleanSf2AudibleGate.h"
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

#ifdef CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST
#include "esp_err.h"
#include "esp_vfs_fat.h"
#endif

#include "platform/EspPsramArena.h"
#include "platform/EspStorageBenchmark.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/VfsRandomAccessReader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

constexpr char kTag[] = "gm_synth";
constexpr std::size_t kMiB = 1024u * 1024u;

void logPsram(const char* phase, const gm_synth::platform::PsramHeapStats& stats) {
    ESP_LOGI(
        kTag,
        "PSRAM %s: total=%u free=%u largest=%u minimum_free=%u bytes",
        phase,
        static_cast<unsigned>(stats.total_bytes),
        static_cast<unsigned>(stats.free_bytes),
        static_cast<unsigned>(stats.largest_free_block_bytes),
        static_cast<unsigned>(stats.minimum_free_bytes));
}

void logInternalHeap(const char* phase) {
    constexpr std::uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(
        kTag,
        "INTERNAL_HEAP %s: total=%u free=%u largest=%u minimum_free=%u bytes",
        phase,
        static_cast<unsigned>(heap_caps_get_total_size(kCaps)),
        static_cast<unsigned>(heap_caps_get_free_size(kCaps)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(kCaps)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(kCaps)));
}

bool storageBetweenCaseIdle(std::size_t chunk_size, void*) {
    ESP_LOGI(kTag, "Storage benchmark case complete: chunk=%u; verifying CPU0 idle progress",
             static_cast<unsigned>(chunk_size));
    return gm_synth::bringup::verifiedQualificationIdleWindow("storage_case");
}

#ifdef CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST
bool mountQemuSf2Fixture() {
    esp_vfs_fat_mount_config_t mount_config{};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 0;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = true;

    const esp_err_t err = esp_vfs_fat_spiflash_mount_ro(
        "/sf2",
        "qemu_sf2",
        &mount_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Benchmark FATFS mount failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(kTag, "Benchmark FATFS mounted: base=/sf2 partition=qemu_sf2");
    return true;
}
#endif

bool runOptionalStorageBenchmark(gm_synth::platform::EspPsramArena& arena) {
#ifdef CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE
    using gm_synth::platform::EspStorageBenchmarkSweepResult;
    using gm_synth::platform::runEspStorageBenchmarkSweep;
    using gm_synth::sf2::Sf2SampleSource;
    using gm_synth::storage::VfsRandomAccessReader;

    const char* path = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH;
    ESP_LOGI(kTag, "Storage benchmark opening VFS path: %s", path);

    VfsRandomAccessReader reader;
    if (!reader.open(path)) {
        ESP_LOGE(
            kTag,
            "Storage benchmark enabled but VFS path cannot be opened: %s (filesystem must already be mounted)",
            path);
        return false;
    }

    ESP_LOGI(
        kTag,
        "Storage benchmark VFS open: path=%s file_bytes=%llu",
        path,
        static_cast<unsigned long long>(reader.size()));

    static Sf2SampleSource source;
    ESP_LOGI(
        kTag,
        "Storage benchmark parser metadata: static_bytes=%u",
        static_cast<unsigned>(sizeof(source)));

    if (!source.begin(reader)) {
        ESP_LOGE(
            kTag,
            "Storage benchmark SF2 parse failed: path=%s error=%u",
            path,
            static_cast<unsigned>(source.lastError()));
        return false;
    }

    ESP_LOGI(
        kTag,
        "Storage benchmark SF2 parsed: playable_samples=%u",
        static_cast<unsigned>(source.sampleCount()));

#ifdef CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST
    if (source.sampleCount() != 67u) {
        ESP_LOGE(
            kTag,
            "Benchmark SF2 fixture sample-count mismatch: expected=67 actual=%u",
            static_cast<unsigned>(source.sampleCount()));
        return false;
    }
#endif

    const std::size_t requested_bytes = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES;
    const std::size_t benchmark_bytes = std::min(
        requested_bytes,
        std::min(arena.capacity(), static_cast<std::size_t>(source.smplPayloadBytes())));

    if (benchmark_bytes == 0u) {
        ESP_LOGE(kTag, "Storage benchmark has no readable smpl payload bytes");
        return false;
    }

    ESP_LOGI(
        kTag,
        "Storage benchmark source: path=%s smpl_offset=%llu smpl_bytes=%u benchmark_bytes=%u",
        path,
        static_cast<unsigned long long>(source.smplPayloadOffset()),
        static_cast<unsigned>(source.smplPayloadBytes()),
        static_cast<unsigned>(benchmark_bytes));

    static EspStorageBenchmarkSweepResult sweep{};
    if (!runEspStorageBenchmarkSweep(
            reader,
            arena.data(),
            arena.capacity(),
            source.smplPayloadOffset(),
            benchmark_bytes,
            &sweep,
            &storageBetweenCaseIdle,
            nullptr)) {
        ESP_LOGE(kTag, "Storage benchmark sweep or between-case idle verification failed");
        return false;
    }
    return true;
#else
    (void)arena;
    return true;
#endif
}

bool requireIdleWindow(const char* phase) {
    if (gm_synth::bringup::verifiedQualificationIdleWindow(phase)) {
        return true;
    }
    ESP_LOGE(kTag, "Physical qualification idle verification FAILED: phase=%s", phase);
    return false;
}

}  // namespace

extern "C" void app_main(void) {
    using gm_synth::platform::EspPsramArena;
    using gm_synth::sample::SampleResidencyManager;

    ESP_LOGI(kTag, "ESP32-S3 GM synth M0/M6 bring-up");
    ESP_LOGI(kTag, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(
        kTag,
        "Reference module: ESP32-S3-WROOM-1-N16R8 (16 MiB flash / 8 MiB octal PSRAM)");

    if (!gm_synth::bringup::initializeQualificationIdleMonitor() ||
        !requireIdleWindow("startup")) {
        ESP_LOGE(kTag, "Qualification idle monitor failed before benchmarks");
        return;
    }

    const std::size_t detected_psram_bytes = esp_psram_get_size();
    ESP_LOGI(
        kTag,
        "Detected physical PSRAM: %u bytes (%.2f MiB)",
        static_cast<unsigned>(detected_psram_bytes),
        static_cast<double>(detected_psram_bytes) / static_cast<double>(kMiB));

    if (detected_psram_bytes < 7u * kMiB) {
        ESP_LOGW(
            kTag,
            "Detected physical PSRAM is below 7 MiB; verify module SKU and octal PSRAM configuration before performance testing");
    }

    const auto before = EspPsramArena::heapStats();
    logPsram("before arena", before);
    logInternalHeap("before arena");

    constexpr std::size_t kRequestedArenaBytes = CONFIG_GM_SYNTH_PCM_ARENA_BYTES;
    ESP_LOGI(
        kTag,
        "Requesting PCM residency arena: %u bytes (%.2f MiB)",
        static_cast<unsigned>(kRequestedArenaBytes),
        static_cast<double>(kRequestedArenaBytes) / static_cast<double>(kMiB));

    static EspPsramArena arena;
    static SampleResidencyManager residency;

    if (before.largest_free_block_bytes < kRequestedArenaBytes) {
        ESP_LOGE(
            kTag,
            "Largest SPIRAM block is already smaller than requested arena: requested=%u largest=%u",
            static_cast<unsigned>(kRequestedArenaBytes),
            static_cast<unsigned>(before.largest_free_block_bytes));
        return;
    }

    if (!arena.allocate(kRequestedArenaBytes)) {
        const auto failed = EspPsramArena::heapStats();
        logPsram("after failed arena allocation", failed);
        logInternalHeap("after failed arena allocation");
        ESP_LOGE(
            kTag,
            "PCM arena allocation failed: requested=%u largest_free=%u",
            static_cast<unsigned>(kRequestedArenaBytes),
            static_cast<unsigned>(failed.largest_free_block_bytes));
        return;
    }

    if ((reinterpret_cast<std::uintptr_t>(arena.data()) &
         (SampleResidencyManager::kAlignment - 1u)) != 0u) {
        ESP_LOGE(kTag, "PCM arena alignment invariant failed");
        arena.reset();
        return;
    }

#ifdef CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST
    if (!mountQemuSf2Fixture()) {
        arena.reset();
        return;
    }
#endif

    if (!runOptionalStorageBenchmark(arena) || !requireIdleWindow("after_storage")) {
        ESP_LOGE(kTag, "Storage qualification FAILED");
        return;
    }

    if (!residency.begin(arena.data(), arena.capacity())) {
        ESP_LOGE(kTag, "SampleResidencyManager failed to initialize over PSRAM arena");
        arena.reset();
        return;
    }

    const auto after = EspPsramArena::heapStats();
    logPsram("after arena", after);
    logInternalHeap("after arena");

    const auto residency_stats = residency.stats();
    ESP_LOGI(
        kTag,
        "Residency ready: capacity=%u used=%u largest_free_span=%u free_spans=%u",
        static_cast<unsigned>(residency_stats.capacity_bytes),
        static_cast<unsigned>(residency_stats.used_bytes),
        static_cast<unsigned>(residency_stats.largest_free_span_bytes),
        static_cast<unsigned>(residency_stats.free_span_count));

    if (!gm_synth::bringup::runOptionalProgramPrepareBenchmark(residency)) {
        ESP_LOGE(kTag, "Program Prepare hardware benchmark FAILED");
        return;
    }

    logPsram("after Program Prepare", EspPsramArena::heapStats());
    logInternalHeap("after Program Prepare");
    if (!requireIdleWindow("after_program_prepare")) {
        return;
    }

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    if (!gm_synth::bringup::runPhysicalCleanSf2AudibleGate(residency)) {
        ESP_LOGE(kTag, "Clean real-SF2 audible qualification FAILED");
        return;
    }
    logPsram("after clean SF2 audio", EspPsramArena::heapStats());
    logInternalHeap("after clean SF2 audio");
    if (!requireIdleWindow("after_clean_sf2")) {
        return;
    }
#endif

    if (!gm_synth::bringup::runOptionalRenderTimingBenchmark()) {
        ESP_LOGE(kTag, "Render timing benchmark FAILED");
        return;
    }
    if (!requireIdleWindow("after_render_benchmark")) {
        return;
    }

    if (!gm_synth::bringup::runOptionalDspStageTimingBenchmark()) {
        ESP_LOGE(kTag, "DSP stage timing benchmark FAILED");
        return;
    }
    if (!requireIdleWindow("after_dsp_stage_benchmark")) {
        return;
    }

    if (!gm_synth::bringup::runOptionalEffectsTimingBenchmark()) {
        ESP_LOGE(kTag, "Effects timing benchmark FAILED");
        return;
    }
    if (!requireIdleWindow("after_effects_benchmark")) {
        return;
    }

    logPsram("after render benchmark", EspPsramArena::heapStats());
    logInternalHeap("after render benchmark");

    if (!gm_synth::bringup::qualificationIdleMonitorHealthy()) {
        ESP_LOGE(kTag, "Qualification idle monitor sticky health is FAILED");
        return;
    }

    const auto final_residency = residency.stats();
    ESP_LOGI(
        kTag,
        "M6_QUAL_MEMORY arena_capacity=%u arena_used=%u arena_high_water=%u arena_largest_free_span=%u resident_samples=%u allocation_failures=%u stale_reference_faults=%u",
        static_cast<unsigned>(final_residency.capacity_bytes),
        static_cast<unsigned>(final_residency.used_bytes),
        static_cast<unsigned>(final_residency.high_water_bytes),
        static_cast<unsigned>(final_residency.largest_free_span_bytes),
        static_cast<unsigned>(final_residency.resident_sample_count),
        static_cast<unsigned>(final_residency.allocation_failures),
        static_cast<unsigned>(final_residency.stale_reference_faults));

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    ESP_LOGI(
        kTag,
        "M6 bring-up qualification passed including clean real-SF2 audio, physical I2S/DMA, ES8311/PA, realtime music stress and full-FX polyphony characterization");
#else
    ESP_LOGI(
        kTag,
        "M6 bring-up core linked successfully; physical I2S qualification is disabled in this target");
#endif
}
