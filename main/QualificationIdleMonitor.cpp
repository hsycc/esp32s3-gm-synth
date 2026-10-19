#include "QualificationIdleMonitor.h"

#include "sdkconfig.h"

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
#include "esp_err.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "qual_idle";
constexpr UBaseType_t kMonitoredCore = 0;
constexpr TickType_t kIdleWindowTicks = 3;

volatile std::uint32_t g_idle0_runs = 0;
bool g_initialized = false;
bool g_healthy = true;

bool idle0MonitorHook() {
    g_idle0_runs = g_idle0_runs + 1u;
    return true;
}

}  // namespace

bool initializeQualificationIdleMonitor() {
    if (g_initialized) {
        return g_healthy;
    }

    const esp_err_t err =
        esp_register_freertos_idle_hook_for_cpu(&idle0MonitorHook, kMonitoredCore);
    if (err != ESP_OK) {
        g_healthy = false;
        ESP_LOGE(kTag, "INIT FAIL core=%u error=%s",
                 static_cast<unsigned>(kMonitoredCore), esp_err_to_name(err));
        return false;
    }

    g_initialized = true;
    ESP_LOGI(kTag, "INIT core=%u window_ticks=%u",
             static_cast<unsigned>(kMonitoredCore),
             static_cast<unsigned>(kIdleWindowTicks));
    return true;
}

bool verifiedQualificationIdleWindow(const char* phase) {
    if (!g_initialized && !initializeQualificationIdleMonitor()) {
        return false;
    }

    const std::uint32_t before = g_idle0_runs;
    vTaskDelay(kIdleWindowTicks);
    const std::uint32_t after = g_idle0_runs;
    const std::uint32_t delta = after - before;
    const bool observed = delta != 0u;
    g_healthy = g_healthy && observed;

    ESP_LOGI(
        kTag,
        "WINDOW phase=%s core=%u ticks=%u idle_before=%u idle_after=%u idle_delta=%u observed=%u sticky_healthy=%u",
        phase != nullptr ? phase : "unknown",
        static_cast<unsigned>(kMonitoredCore),
        static_cast<unsigned>(kIdleWindowTicks),
        static_cast<unsigned>(before),
        static_cast<unsigned>(after),
        static_cast<unsigned>(delta),
        observed ? 1u : 0u,
        g_healthy ? 1u : 0u);

    return g_healthy;
}

bool qualificationIdleMonitorHealthy() {
    return g_healthy;
}

}  // namespace gm_synth::bringup

#else

namespace gm_synth::bringup {

bool initializeQualificationIdleMonitor() {
    return true;
}

bool verifiedQualificationIdleWindow(const char*) {
    return true;
}

bool qualificationIdleMonitorHealthy() {
    return true;
}

}  // namespace gm_synth::bringup

#endif
