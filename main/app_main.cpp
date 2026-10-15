#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

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

void runOptionalStorageBenchmark(gm_synth::platform::EspPsramArena& arena) {
#ifdef CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE
    using gm_synth::platform::EspStorageBenchmarkSweepResult;
    using gm_synth::platform::runEspStorageBenchmarkSweep;
    using gm_synth::sf2::Sf2SampleSource;
    using gm_synth::storage::VfsRandomAccessReader;

    const char* path = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH;
    VfsRandomAccessReader reader;
    if (!reader.open(path)) {
        ESP_LOGE(
            kTag,
            "Storage benchmark enabled but VFS path cannot be opened: %s (filesystem must already be mounted)",
            path);
        return;
    }

    Sf2SampleSource source;
    if (!source.begin(reader)) {
        ESP_LOGE(
            kTag,
            "Storage benchmark SF2 parse failed: path=%s error=%u",
            path,
            static_cast<unsigned>(source.lastError()));
        return;
    }

    const std::size_t requested_bytes = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES;
    const std::size_t benchmark_bytes = std::min(
        requested_bytes,
        std::min(arena.capacity(), static_cast<std::size_t>(source.smplPayloadBytes())));

    if (benchmark_bytes == 0u) {
        ESP_LOGE(kTag, "Storage benchmark has no readable smpl payload bytes");
        return;
    }

    ESP_LOGI(
        kTag,
        "Storage benchmark source: path=%s smpl_offset=%llu smpl_bytes=%u benchmark_bytes=%u",
        path,
        static_cast<unsigned long long>(source.smplPayloadOffset()),
        static_cast<unsigned>(source.smplPayloadBytes()),
        static_cast<unsigned>(benchmark_bytes));

    EspStorageBenchmarkSweepResult sweep{};
    if (!runEspStorageBenchmarkSweep(
            reader,
            arena.data(),
            arena.capacity(),
            source.smplPayloadOffset(),
            benchmark_bytes,
            &sweep)) {
        ESP_LOGW(kTag, "One or more storage benchmark chunk-size passes failed");
    }
#else
    (void)arena;
#endif
}

}  // namespace

extern "C" void app_main(void) {
    using gm_synth::platform::EspPsramArena;
    using gm_synth::sample::SampleResidencyManager;

    ESP_LOGI(kTag, "ESP32-S3 GM synth M0/M3 bring-up");
    ESP_LOGI(kTag, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(
        kTag,
        "Reference module: ESP32-S3-WROOM-1-N16R8 (16 MiB flash / 8 MiB octal PSRAM)");

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

    // Bring-up benchmark intentionally runs before residency metadata starts using
    // the arena. It overwrites PCM bytes only; the later residency initialization
    // owns allocation metadata and future sample loads overwrite these bytes.
    runOptionalStorageBenchmark(arena);

    if (!residency.begin(arena.data(), arena.capacity())) {
        ESP_LOGE(kTag, "SampleResidencyManager failed to initialize over PSRAM arena");
        arena.reset();
        return;
    }

    const auto after = EspPsramArena::heapStats();
    logPsram("after arena", after);

    const auto residency_stats = residency.stats();
    ESP_LOGI(
        kTag,
        "Residency ready: capacity=%u used=%u largest_free_span=%u free_spans=%u",
        static_cast<unsigned>(residency_stats.capacity_bytes),
        static_cast<unsigned>(residency_stats.used_bytes),
        static_cast<unsigned>(residency_stats.largest_free_span_bytes),
        static_cast<unsigned>(residency_stats.free_span_count));

    ESP_LOGI(
        kTag,
        "M3 core linked successfully; storage/I2S/Voice DSP are intentionally not started in this bring-up target");
}
