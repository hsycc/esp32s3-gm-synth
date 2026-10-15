#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_system.h"

#include "platform/EspPsramArena.h"
#include "sample/SampleResidencyManager.h"

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

}  // namespace

extern "C" void app_main(void) {
    using gm_synth::platform::EspPsramArena;
    using gm_synth::sample::SampleResidencyManager;

    ESP_LOGI(kTag, "ESP32-S3 GM synth M0/M3 bring-up");
    ESP_LOGI(kTag, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(
        kTag,
        "Reference module: ESP32-S3-WROOM-1-N16R8 (16 MiB flash / 8 MiB octal PSRAM)");

    const auto before = EspPsramArena::heapStats();
    logPsram("before arena", before);

    if (before.total_bytes < 7u * kMiB) {
        ESP_LOGW(
            kTag,
            "Detected SPIRAM-capable heap is below 7 MiB; verify module SKU and PSRAM mode before performance testing");
    }

    constexpr std::size_t kRequestedArenaBytes = CONFIG_GM_SYNTH_PCM_ARENA_BYTES;
    ESP_LOGI(
        kTag,
        "Requesting PCM residency arena: %u bytes (%.2f MiB)",
        static_cast<unsigned>(kRequestedArenaBytes),
        static_cast<double>(kRequestedArenaBytes) / static_cast<double>(kMiB));

    static EspPsramArena arena;
    static SampleResidencyManager residency;

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
