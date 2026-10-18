#include "I2sDmaQualification.h"

#include "KorvoV4Es8311.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform/EspRealtimeClock.h"
#include "realtime/ScheduledAudioBlockRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
constexpr std::size_t kSamplesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock;
constexpr std::size_t kBytesPerBlock = kSamplesPerBlock * sizeof(std::int16_t);
constexpr std::uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

static_assert(kSamplesPerBlock == kFramesPerBlock * 2,
              "physical I2S qualification requires interleaved stereo renderer output");

struct LatencyStats {
    std::uint32_t min_us = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t max_us = 0;
    std::uint64_t total_us = 0;
    std::uint32_t samples = 0;

    void add(std::uint32_t value) {
        min_us = std::min(min_us, value);
        max_us = std::max(max_us, value);
        total_us += value;
        ++samples;
    }

    std::uint32_t average() const {
        return samples == 0 ? 0 : static_cast<std::uint32_t>(total_us / samples);
    }

    std::uint32_t minimum() const {
        return samples == 0 ? 0 : min_us;
    }
};

struct InternalHeapSnapshot {
    std::uint32_t free_bytes = 0;
    std::uint32_t largest_block_bytes = 0;
    std::uint32_t minimum_free_bytes = 0;
};

struct CallbackStats {
    volatile std::uint32_t sent_events = 0;
    volatile std::uint32_t send_queue_overflows = 0;
};

struct TaskContext {
    realtime::ScheduledAudioBlockRenderer* renderer = nullptr;
    std::uint64_t block_start_frame = 0;
    TaskHandle_t owner_task = nullptr;
    bool success = false;
};

InternalHeapSnapshot internalHeapSnapshot() {
    InternalHeapSnapshot result{};
    result.free_bytes = static_cast<std::uint32_t>(
        heap_caps_get_free_size(kInternalHeapCaps));
    result.largest_block_bytes = static_cast<std::uint32_t>(
        heap_caps_get_largest_free_block(kInternalHeapCaps));
    result.minimum_free_bytes = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(kInternalHeapCaps));
    return result;
}

void logInternalHeap(const char* phase, const InternalHeapSnapshot& stats) {
    ESP_LOGI(
        kTag,
        "i2s_qual: INTERNAL_HEAP phase=%s free=%u largest=%u minimum_free=%u bytes",
        phase,
        static_cast<unsigned>(stats.free_bytes),
        static_cast<unsigned>(stats.largest_block_bytes),
        static_cast<unsigned>(stats.minimum_free_bytes));
}

std::uint32_t dmaWindowUs() {
    const std::uint64_t frames =
        static_cast<std::uint64_t>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM) *
        CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM;
    return static_cast<std::uint32_t>(
        (frames * 1000000ull + kSampleRate - 1u) / kSampleRate);
}

std::uint32_t elapsedUs(std::uint64_t start_us, std::uint64_t end_us) {
    if (end_us < start_us) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_us - start_us,
        std::numeric_limits<std::uint32_t>::max()));
}

void downmixMonoToStereoSlots(
    std::array<std::int16_t, kSamplesPerBlock>& output) {
    for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
        const std::size_t index = frame * 2;
        const std::int32_t sum =
            static_cast<std::int32_t>(output[index]) +
            static_cast<std::int32_t>(output[index + 1]);
        const std::int16_t mono = static_cast<std::int16_t>(sum / 2);
        output[index] = mono;
        output[index + 1] = mono;
    }
}

std::uint64_t updateChecksum(
    std::uint64_t checksum,
    const std::array<std::int16_t, kSamplesPerBlock>& output) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int16_t value : output) {
        checksum ^= static_cast<std::uint16_t>(value);
        checksum *= kFnvPrime;
    }
    return checksum;
}

bool IRAM_ATTR onSent(
    i2s_chan_handle_t,
    i2s_event_data_t*,
    void* user_context) {
    auto* stats = static_cast<CallbackStats*>(user_context);
    stats->sent_events = stats->sent_events + 1u;
    return false;
}

bool IRAM_ATTR onSendQueueOverflow(
    i2s_chan_handle_t,
    i2s_event_data_t*,
    void* user_context) {
    auto* stats = static_cast<CallbackStats*>(user_context);
    stats->send_queue_overflows = stats->send_queue_overflows + 1u;
    return false;
}

bool runI2sTask(TaskContext& context) {
    if (context.renderer == nullptr) {
        ESP_LOGE(kTag, "i2s_qual: renderer is null");
        return false;
    }

    if (CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO < 0 ||
        CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO < 0 ||
        CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO < 0 ||
        CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO < 0) {
        ESP_LOGE(
            kTag,
            "i2s_qual: Korvo qualification requires real MCLK/BCLK/WS/DOUT GPIOs");
        return false;
    }

    const InternalHeapSnapshot task_entry_heap = internalHeapSnapshot();
    logInternalHeap("task_entry", task_entry_heap);

    // Anti-pop order begins here: PA OFF -> codec configured + muted. The codec
    // stays muted until the DMA ring contains silence and I2S clocks are live.
    KorvoV4Es8311 codec;
    if (!codec.prepareMuted()) {
        ESP_LOGE(kTag, "i2s_qual: ES8311 muted preparation failed");
        return false;
    }

    i2s_chan_handle_t tx_channel = nullptr;
    bool channel_enabled = false;
    const auto cleanup = [&]() {
        // Keep clocks alive while the codec is first disconnected from the PA,
        // muted and powered down. Only then stop/delete the I2S channel.
        codec.shutdown();
        if (tx_channel != nullptr) {
            if (channel_enabled) {
                const esp_err_t disable_err = i2s_channel_disable(tx_channel);
                if (disable_err != ESP_OK) {
                    ESP_LOGW(kTag, "i2s_qual: disable failed: %s",
                             esp_err_to_name(disable_err));
                }
            }
            const esp_err_t delete_err = i2s_del_channel(tx_channel);
            if (delete_err != ESP_OK) {
                ESP_LOGW(kTag, "i2s_qual: delete failed: %s",
                         esp_err_to_name(delete_err));
            }
            tx_channel = nullptr;
            channel_enabled = false;
        }
    };

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_AUTO,
        I2S_ROLE_MASTER);
    channel_config.dma_desc_num = CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM;
    channel_config.dma_frame_num = CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&channel_config, &tx_channel, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: i2s_new_channel failed: %s", esp_err_to_name(err));
        cleanup();
        return false;
    }

    // Initialize the ESP-IDF config in assignments instead of relying on C++
    // designated-initializer support/order. This keeps the physical build
    // compatible with the compiler mode selected by ESP-IDF.
    i2s_std_config_t standard_config{};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    standard_config.gpio_cfg.mclk =
        static_cast<gpio_num_t>(CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO);
    standard_config.gpio_cfg.bclk =
        static_cast<gpio_num_t>(CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO);
    standard_config.gpio_cfg.ws =
        static_cast<gpio_num_t>(CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO);
    standard_config.gpio_cfg.dout =
        static_cast<gpio_num_t>(CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO);
    standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    standard_config.gpio_cfg.invert_flags.mclk_inv = false;
    standard_config.gpio_cfg.invert_flags.bclk_inv = false;
    standard_config.gpio_cfg.invert_flags.ws_inv = false;

    // ES8311 reference clock table has an exact 44.1 kHz entry at 256*Fs.
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(tx_channel, &standard_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        cleanup();
        return false;
    }

    i2s_chan_info_t channel_info{};
    err = i2s_channel_get_info(tx_channel, &channel_info);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: i2s_channel_get_info failed: %s",
                 esp_err_to_name(err));
        cleanup();
        return false;
    }
    const InternalHeapSnapshot dma_ready_heap = internalHeapSnapshot();
    logInternalHeap("dma_ready", dma_ready_heap);

    CallbackStats callback_stats{};
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = onSent;
    callbacks.on_send_q_ovf = onSendQueueOverflow;
    err = i2s_channel_register_event_callback(tx_channel, &callbacks, &callback_stats);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: callback registration failed: %s",
                 esp_err_to_name(err));
        cleanup();
        return false;
    }

    // PA OFF + codec muted are already guaranteed. Fill all available DMA
    // buffers with stereo-slot silence before starting clocks.
    std::array<std::int16_t, kSamplesPerBlock> silence{};
    std::size_t total_preloaded_bytes = 0;
    for (std::size_t attempt = 0;
         attempt < static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM) + 1u;
         ++attempt) {
        std::size_t loaded_bytes = 0;
        err = i2s_channel_preload_data(
            tx_channel, silence.data(), sizeof(silence), &loaded_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "i2s_qual: DMA preload failed: %s", esp_err_to_name(err));
            cleanup();
            return false;
        }
        total_preloaded_bytes += loaded_bytes;
        if (loaded_bytes < sizeof(silence)) {
            break;
        }
    }

    // Start MCLK/BCLK/LRCK only after silence is resident in DMA.
    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: i2s_channel_enable failed: %s", esp_err_to_name(err));
        cleanup();
        return false;
    }
    channel_enabled = true;

    // Complete anti-pop startup: clocks live -> codec unmute -> PA ON.
    if (!codec.enableOutput()) {
        ESP_LOGE(kTag, "i2s_qual: ES8311 output enable failed");
        cleanup();
        return false;
    }

    ESP_LOGI(
        kTag,
        "i2s_qual: START sample_rate=%u frames_per_block=%u bytes_per_block=%u frame_format=stereo16 mono_policy=avg_lr_duplicate dma_desc=%u dma_frames=%u dma_window_us=%u driver_dma_bytes=%u preloaded_bytes=%u bclk_hz=%u mclk_hz=%u bclk_gpio=%d ws_gpio=%d dout_gpio=%d mclk_gpio=%d task_core=%d task_priority=%u",
        static_cast<unsigned>(kSampleRate),
        static_cast<unsigned>(kFramesPerBlock),
        static_cast<unsigned>(kBytesPerBlock),
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM),
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM),
        static_cast<unsigned>(dmaWindowUs()),
        static_cast<unsigned>(channel_info.total_dma_buf_size),
        static_cast<unsigned>(total_preloaded_bytes),
        static_cast<unsigned>(channel_info.bclk_hz),
        static_cast<unsigned>(channel_info.mclk_hz),
        CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_TASK_CORE,
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_TASK_PRIORITY));

    std::array<std::int16_t, kSamplesPerBlock> output{};
    std::uint64_t block_start_frame = context.block_start_frame;
    const auto renderAndWrite = [&](bool measure,
                                    LatencyStats* render_stats,
                                    LatencyStats* write_stats,
                                    LatencyStats* cycle_stats,
                                    std::uint32_t* render_deadline_misses,
                                    std::uint32_t* cycle_over_budget,
                                    std::uint32_t* producer_stalls,
                                    std::uint32_t* write_timeouts,
                                    std::uint32_t* partial_writes,
                                    std::uint64_t* checksum) -> bool {
        const std::uint64_t cycle_start_us = platform::espRealtimeNowUs(nullptr);
        context.renderer->render(block_start_frame, output.data());
        downmixMonoToStereoSlots(output);
        const std::uint64_t render_end_us = platform::espRealtimeNowUs(nullptr);
        block_start_frame += kFramesPerBlock;

        std::size_t bytes_written = 0;
        const esp_err_t write_err = i2s_channel_write(
            tx_channel,
            output.data(),
            sizeof(output),
            &bytes_written,
            CONFIG_GM_SYNTH_I2S_QUAL_WRITE_TIMEOUT_MS);
        const std::uint64_t write_end_us = platform::espRealtimeNowUs(nullptr);

        if (write_err == ESP_ERR_TIMEOUT) {
            ++(*write_timeouts);
            return false;
        }
        if (write_err != ESP_OK) {
            ESP_LOGE(kTag, "i2s_qual: write failed: %s", esp_err_to_name(write_err));
            return false;
        }
        if (bytes_written != sizeof(output)) {
            ++(*partial_writes);
            return false;
        }

        if (measure) {
            const std::uint32_t render_us = elapsedUs(cycle_start_us, render_end_us);
            const std::uint32_t write_us = elapsedUs(render_end_us, write_end_us);
            const std::uint32_t cycle_us = elapsedUs(cycle_start_us, write_end_us);
            render_stats->add(render_us);
            write_stats->add(write_us);
            cycle_stats->add(cycle_us);
            if (render_us > kBlockBudgetUs) {
                ++(*render_deadline_misses);
            }
            if (cycle_us > kBlockBudgetUs) {
                ++(*cycle_over_budget);
            }
            if (cycle_us > dmaWindowUs()) {
                ++(*producer_stalls);
            }
            *checksum = updateChecksum(*checksum, output);
        }
        return true;
    };

    std::uint32_t render_deadline_misses = 0;
    std::uint32_t cycle_over_budget = 0;
    std::uint32_t producer_stalls = 0;
    std::uint32_t write_timeouts = 0;
    std::uint32_t partial_writes = 0;
    std::uint64_t checksum = 1469598103934665603ull;
    LatencyStats render_stats{};
    LatencyStats write_stats{};
    LatencyStats cycle_stats{};

    const std::uint64_t clipped_before = context.renderer->stats().clipped_samples;

    for (std::size_t block = 0;
         block < static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_WARMUP_BLOCKS);
         ++block) {
        if (!renderAndWrite(false, &render_stats, &write_stats, &cycle_stats,
                            &render_deadline_misses, &cycle_over_budget,
                            &producer_stalls, &write_timeouts, &partial_writes,
                            &checksum)) {
            cleanup();
            return false;
        }
    }

    for (std::size_t block = 0;
         block < static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS);
         ++block) {
        if (!renderAndWrite(true, &render_stats, &write_stats, &cycle_stats,
                            &render_deadline_misses, &cycle_over_budget,
                            &producer_stalls, &write_timeouts, &partial_writes,
                            &checksum)) {
            cleanup();
            return false;
        }
    }

    const std::uint64_t clipped_samples =
        context.renderer->stats().clipped_samples - clipped_before;
    const UBaseType_t stack_high_water_bytes = uxTaskGetStackHighWaterMark(nullptr);
    const InternalHeapSnapshot before_cleanup_heap = internalHeapSnapshot();
    logInternalHeap("before_cleanup", before_cleanup_heap);

    ESP_LOGI(
        kTag,
        "i2s_qual: RESULT blocks=%u render_us[min/avg/max]=%u/%u/%u write_wait_us[min/avg/max]=%u/%u/%u cycle_us[min/avg/max]=%u/%u/%u render_deadline_misses=%u cycle_over_budget=%u producer_stalls_over_dma_window=%u write_timeouts=%u partial_writes=%u sent_callbacks=%u send_event_queue_overflows=%u clipped_samples=%llu stack_high_water_bytes=%u checksum=%016llx",
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS),
        static_cast<unsigned>(render_stats.minimum()),
        static_cast<unsigned>(render_stats.average()),
        static_cast<unsigned>(render_stats.max_us),
        static_cast<unsigned>(write_stats.minimum()),
        static_cast<unsigned>(write_stats.average()),
        static_cast<unsigned>(write_stats.max_us),
        static_cast<unsigned>(cycle_stats.minimum()),
        static_cast<unsigned>(cycle_stats.average()),
        static_cast<unsigned>(cycle_stats.max_us),
        static_cast<unsigned>(render_deadline_misses),
        static_cast<unsigned>(cycle_over_budget),
        static_cast<unsigned>(producer_stalls),
        static_cast<unsigned>(write_timeouts),
        static_cast<unsigned>(partial_writes),
        static_cast<unsigned>(callback_stats.sent_events),
        static_cast<unsigned>(callback_stats.send_queue_overflows),
        static_cast<unsigned long long>(clipped_samples),
        static_cast<unsigned>(stack_high_water_bytes),
        static_cast<unsigned long long>(checksum));

    const bool pass =
        render_deadline_misses == 0u &&
        producer_stalls == 0u &&
        write_timeouts == 0u &&
        partial_writes == 0u &&
        callback_stats.send_queue_overflows == 0u &&
        clipped_samples == 0u &&
        stack_high_water_bytes >= CONFIG_GM_SYNTH_I2S_QUAL_MIN_STACK_HEADROOM_BYTES;

    cleanup();
    const InternalHeapSnapshot after_cleanup_heap = internalHeapSnapshot();
    logInternalHeap("after_cleanup", after_cleanup_heap);
    ESP_LOGI(
        kTag,
        "i2s_qual: MEMORY task_entry_free=%u dma_ready_free=%u before_cleanup_free=%u after_cleanup_free=%u dma_alloc_delta=%d cleanup_recovered=%d",
        static_cast<unsigned>(task_entry_heap.free_bytes),
        static_cast<unsigned>(dma_ready_heap.free_bytes),
        static_cast<unsigned>(before_cleanup_heap.free_bytes),
        static_cast<unsigned>(after_cleanup_heap.free_bytes),
        static_cast<int>(task_entry_heap.free_bytes) - static_cast<int>(dma_ready_heap.free_bytes),
        static_cast<int>(after_cleanup_heap.free_bytes) - static_cast<int>(before_cleanup_heap.free_bytes));
    ESP_LOGI(kTag, "i2s_qual: %s", pass ? "PASS" : "FAIL");
    return pass;
}

void i2sTaskEntry(void* argument) {
    auto* context = static_cast<TaskContext*>(argument);
    context->success = runI2sTask(*context);
    xTaskNotifyGive(context->owner_task);
    vTaskDelete(nullptr);
}

}  // namespace

bool runOptionalI2sDmaQualification(
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t block_start_frame) {
    TaskContext context{};
    context.renderer = &renderer;
    context.block_start_frame = block_start_frame;
    context.owner_task = xTaskGetCurrentTaskHandle();

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        i2sTaskEntry,
        "gm_i2s_qual",
        CONFIG_GM_SYNTH_I2S_QUAL_TASK_STACK_BYTES,
        &context,
        CONFIG_GM_SYNTH_I2S_QUAL_TASK_PRIORITY,
        &task,
        CONFIG_GM_SYNTH_I2S_QUAL_TASK_CORE);
    if (created != pdPASS || task == nullptr) {
        ESP_LOGE(kTag, "i2s_qual: failed to create audio qualification task");
        return false;
    }

    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Let the pinned core's idle task reclaim the dynamically-created task
    // stack/TCB before subsequent memory snapshots are taken.
    vTaskDelay(1);
    return context.success;
}

}  // namespace gm_synth::bringup
