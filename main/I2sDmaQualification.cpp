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
#include "realtime/RealtimeHealthTracker.h"
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
constexpr std::uint32_t kExpectedBclkHz = kSampleRate * 16u * 2u;
constexpr std::uint32_t kExpectedMclkHz = kSampleRate * 256u;
constexpr std::uint32_t kClockTolerancePercent = 1u;
constexpr std::size_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
constexpr std::size_t kRendererSamplesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock;
constexpr std::size_t kI2sSamplesPerBlock = kFramesPerBlock;
constexpr std::size_t kBytesPerBlock = kI2sSamplesPerBlock * sizeof(std::int16_t);
constexpr std::uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr std::uint32_t kReferenceToneHz = 441;
constexpr std::size_t kReferenceTonePeriodFrames = kSampleRate / kReferenceToneHz;
constexpr std::int16_t kReferenceTonePeak = 8192;
constexpr std::size_t kReferenceToneBlocks = 768;

static_assert(kRendererSamplesPerBlock == kFramesPerBlock * 2,
              "physical I2S qualification expects interleaved stereo renderer output");
static_assert(kSampleRate % kReferenceToneHz == 0,
              "qualification tone frequency must divide sample rate exactly");

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

struct PcmStats {
    std::uint32_t peak_abs = 0;
    std::uint64_t total_abs = 0;
    std::uint64_t samples = 0;
    std::uint64_t nonzero_samples = 0;

    void add(const std::array<std::int16_t, kI2sSamplesPerBlock>& values) {
        for (const std::int16_t value : values) {
            const std::int32_t widened = static_cast<std::int32_t>(value);
            const std::uint32_t magnitude = static_cast<std::uint32_t>(
                widened < 0 ? -widened : widened);
            peak_abs = std::max(peak_abs, magnitude);
            total_abs += magnitude;
            ++samples;
            if (value != 0) {
                ++nonzero_samples;
            }
        }
    }

    std::uint32_t averageAbs() const {
        return samples == 0 ? 0 : static_cast<std::uint32_t>(total_abs / samples);
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

std::uint32_t addUs(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(left) + right,
        std::numeric_limits<std::uint32_t>::max()));
}

bool clockWithinTolerance(std::uint32_t actual_hz, std::uint32_t expected_hz) {
    if (actual_hz == 0u || expected_hz == 0u) {
        return false;
    }
    const std::uint64_t delta = actual_hz >= expected_hz
        ? static_cast<std::uint64_t>(actual_hz - expected_hz)
        : static_cast<std::uint64_t>(expected_hz - actual_hz);
    return delta * 100u <=
        static_cast<std::uint64_t>(expected_hz) * kClockTolerancePercent;
}

void downmixToMono(
    const std::array<std::int16_t, kRendererSamplesPerBlock>& stereo,
    std::array<std::int16_t, kI2sSamplesPerBlock>& mono) {
    for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
        const std::size_t index = frame * 2;
        const std::int32_t sum =
            static_cast<std::int32_t>(stereo[index]) +
            static_cast<std::int32_t>(stereo[index + 1]);
        mono[frame] = static_cast<std::int16_t>(sum / 2);
    }
}

void fillReferenceTone(
    std::array<std::int16_t, kI2sSamplesPerBlock>& mono,
    std::uint64_t start_frame) {
    for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
        const std::size_t phase = static_cast<std::size_t>(
            (start_frame + frame) % kReferenceTonePeriodFrames);
        const std::size_t folded = phase <= (kReferenceTonePeriodFrames / 2u)
            ? phase
            : kReferenceTonePeriodFrames - phase;
        const std::int32_t centered =
            static_cast<std::int32_t>(folded * 2u) -
            static_cast<std::int32_t>(kReferenceTonePeriodFrames / 2u);
        mono[frame] = static_cast<std::int16_t>(
            centered * static_cast<std::int32_t>(kReferenceTonePeak) /
            static_cast<std::int32_t>(kReferenceTonePeriodFrames / 2u));
    }
}

template <std::size_t N>
std::uint64_t updateChecksum(
    std::uint64_t checksum,
    const std::array<std::int16_t, N>& output) {
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

    KorvoV4Es8311 codec;
    if (!codec.prepareMuted()) {
        ESP_LOGE(kTag, "i2s_qual: ES8311 muted control preparation failed");
        return false;
    }

    i2s_chan_handle_t tx_channel = nullptr;
    bool channel_enabled = false;
    const auto cleanup = [&]() {
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

    i2s_std_config_t standard_config{};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO);
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

    const bool bclk_ok = clockWithinTolerance(channel_info.bclk_hz, kExpectedBclkHz);
    const bool mclk_ok = clockWithinTolerance(channel_info.mclk_hz, kExpectedMclkHz);
    if (!bclk_ok || !mclk_ok) {
        ESP_LOGE(
            kTag,
            "i2s_qual: CLOCK_GATE FAIL expected_bclk=%u actual_bclk=%u expected_mclk=%u actual_mclk=%u tolerance_percent=%u",
            static_cast<unsigned>(kExpectedBclkHz),
            static_cast<unsigned>(channel_info.bclk_hz),
            static_cast<unsigned>(kExpectedMclkHz),
            static_cast<unsigned>(channel_info.mclk_hz),
            static_cast<unsigned>(kClockTolerancePercent));
        cleanup();
        return false;
    }
    ESP_LOGI(
        kTag,
        "i2s_qual: CLOCK_GATE PASS expected_bclk=%u actual_bclk=%u expected_mclk=%u actual_mclk=%u tolerance_percent=%u",
        static_cast<unsigned>(kExpectedBclkHz),
        static_cast<unsigned>(channel_info.bclk_hz),
        static_cast<unsigned>(kExpectedMclkHz),
        static_cast<unsigned>(channel_info.mclk_hz),
        static_cast<unsigned>(kClockTolerancePercent));

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

    std::array<std::int16_t, kI2sSamplesPerBlock> silence{};
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

    if (channel_info.total_dma_buf_size == 0u ||
        total_preloaded_bytes != static_cast<std::size_t>(channel_info.total_dma_buf_size)) {
        ESP_LOGE(
            kTag,
            "i2s_qual: DMA_PRELOAD_GATE FAIL driver_dma_bytes=%u preloaded_bytes=%u",
            static_cast<unsigned>(channel_info.total_dma_buf_size),
            static_cast<unsigned>(total_preloaded_bytes));
        cleanup();
        return false;
    }
    ESP_LOGI(
        kTag,
        "i2s_qual: DMA_PRELOAD_GATE PASS driver_dma_bytes=%u preloaded_bytes=%u",
        static_cast<unsigned>(channel_info.total_dma_buf_size),
        static_cast<unsigned>(total_preloaded_bytes));

    const std::uint32_t dma_window_us = dmaWindowUs();
    ESP_LOGI(
        kTag,
        "i2s_qual: ARMED sample_rate=%u frames_per_block=%u bytes_per_block=%u frame_format=mono16_philips mono_policy=avg_lr dma_desc=%u dma_frames=%u dma_window_us=%u driver_dma_bytes=%u preloaded_bytes=%u reference_tone_hz=%u reference_tone_blocks=%u reference_tone_peak=%d bclk_hz=%u mclk_hz=%u bclk_gpio=%d ws_gpio=%d dout_gpio=%d mclk_gpio=%d task_core=%d task_priority=%u",
        static_cast<unsigned>(kSampleRate),
        static_cast<unsigned>(kFramesPerBlock),
        static_cast<unsigned>(kBytesPerBlock),
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM),
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM),
        static_cast<unsigned>(dma_window_us),
        static_cast<unsigned>(channel_info.total_dma_buf_size),
        static_cast<unsigned>(total_preloaded_bytes),
        static_cast<unsigned>(kReferenceToneHz),
        static_cast<unsigned>(kReferenceToneBlocks),
        static_cast<int>(kReferenceTonePeak),
        static_cast<unsigned>(channel_info.bclk_hz),
        static_cast<unsigned>(channel_info.mclk_hz),
        CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO,
        CONFIG_GM_SYNTH_I2S_QUAL_TASK_CORE,
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_TASK_PRIORITY));

    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_qual: i2s_channel_enable failed: %s", esp_err_to_name(err));
        cleanup();
        return false;
    }
    channel_enabled = true;

    if (!codec.startMutedWithClocks()) {
        ESP_LOGE(kTag, "i2s_qual: ES8311 clocked muted start failed");
        cleanup();
        return false;
    }

    const auto writeExact = [&](const std::int16_t* samples,
                                std::size_t sample_count,
                                const char* phase) -> bool {
        std::size_t bytes_written = 0;
        const std::size_t bytes = sample_count * sizeof(std::int16_t);
        const esp_err_t write_err = i2s_channel_write(
            tx_channel,
            samples,
            bytes,
            &bytes_written,
            CONFIG_GM_SYNTH_I2S_QUAL_WRITE_TIMEOUT_MS);
        if (write_err != ESP_OK) {
            ESP_LOGE(kTag, "i2s_qual: %s write failed: %s",
                     phase, esp_err_to_name(write_err));
            return false;
        }
        if (bytes_written != bytes) {
            ESP_LOGE(kTag, "i2s_qual: %s partial write expected=%u actual=%u",
                     phase,
                     static_cast<unsigned>(bytes),
                     static_cast<unsigned>(bytes_written));
            return false;
        }
        return true;
    };

    const std::size_t live_prime_blocks =
        static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM) * 2u;
    for (std::size_t block = 0; block < live_prime_blocks; ++block) {
        if (!writeExact(silence.data(), silence.size(), "live_prime")) {
            cleanup();
            return false;
        }
    }

    const std::uint32_t startup_sent_callbacks = callback_stats.sent_events;
    const std::uint32_t startup_send_queue_overflows = callback_stats.send_queue_overflows;

    if (!codec.enableOutput()) {
        ESP_LOGE(kTag, "i2s_qual: ES8311 output enable/readback failed");
        cleanup();
        return false;
    }

    std::array<std::int16_t, kI2sSamplesPerBlock> mono_output{};
    std::uint64_t tone_frame = 0;
    std::uint64_t tone_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kReferenceToneBlocks; ++block) {
        fillReferenceTone(mono_output, tone_frame);
        tone_frame += kFramesPerBlock;
        tone_checksum = updateChecksum(tone_checksum, mono_output);
        if (!writeExact(mono_output.data(), mono_output.size(), "reference_tone")) {
            cleanup();
            return false;
        }
    }
    const std::uint32_t tone_end_sent_callbacks = callback_stats.sent_events;
    const std::uint32_t tone_end_send_queue_overflows = callback_stats.send_queue_overflows;
    const std::uint32_t tone_sent_callbacks =
        tone_end_sent_callbacks - startup_sent_callbacks;
    const std::uint32_t tone_send_queue_overflows =
        tone_end_send_queue_overflows - startup_send_queue_overflows;

    std::array<std::int16_t, kRendererSamplesPerBlock> render_output{};
    std::uint64_t block_start_frame = context.block_start_frame;
    PcmStats pcm_stats{};
    realtime::RealtimeHealthTracker health_tracker(kBlockBudgetUs, dma_window_us);

    const auto renderAndWrite = [&](bool measure,
                                    LatencyStats* render_stats,
                                    LatencyStats* producer_work_stats,
                                    LatencyStats* diagnostic_overhead_stats,
                                    LatencyStats* write_stats,
                                    LatencyStats* cycle_stats,
                                    std::uint32_t* cycle_over_budget,
                                    std::uint32_t* producer_stalls,
                                    std::uint32_t* write_timeouts,
                                    std::uint32_t* partial_writes,
                                    std::uint64_t* checksum) -> bool {
        const std::uint64_t cycle_start_us = platform::espRealtimeNowUs(nullptr);
        const bool profiled_render = context.renderer->worstBlockProfilingEnabled();
        context.renderer->render(block_start_frame, render_output.data());
        const std::uint64_t renderer_return_us = platform::espRealtimeNowUs(nullptr);

        const std::uint32_t renderer_call_us = elapsedUs(cycle_start_us, renderer_return_us);
        const std::uint32_t render_us = profiled_render
            ? context.renderer->lastProfiledRenderUs()
            : renderer_call_us;
        const std::uint32_t diagnostic_overhead_us = renderer_call_us >= render_us
            ? renderer_call_us - render_us
            : 0u;

        const std::uint64_t downmix_start_us = platform::espRealtimeNowUs(nullptr);
        downmixToMono(render_output, mono_output);
        const std::uint64_t producer_work_end_us = platform::espRealtimeNowUs(nullptr);
        const std::uint32_t downmix_us = elapsedUs(downmix_start_us, producer_work_end_us);
        const std::uint32_t producer_work_us = addUs(render_us, downmix_us);
        block_start_frame += kFramesPerBlock;

        std::size_t bytes_written = 0;
        const esp_err_t write_err = i2s_channel_write(
            tx_channel,
            mono_output.data(),
            sizeof(mono_output),
            &bytes_written,
            CONFIG_GM_SYNTH_I2S_QUAL_WRITE_TIMEOUT_MS);
        const std::uint64_t write_end_us = platform::espRealtimeNowUs(nullptr);

        if (write_err == ESP_ERR_TIMEOUT) {
            ++(*write_timeouts);
            return false;
        }
        if (write_err != ESP_OK) {
            ESP_LOGE(kTag, "i2s_qual: synth write failed: %s", esp_err_to_name(write_err));
            return false;
        }
        if (bytes_written != sizeof(mono_output)) {
            ++(*partial_writes);
            return false;
        }

        if (measure) {
            const std::uint32_t write_us = elapsedUs(producer_work_end_us, write_end_us);
            const std::uint32_t cycle_us = elapsedUs(cycle_start_us, write_end_us);
            render_stats->add(render_us);
            producer_work_stats->add(producer_work_us);
            diagnostic_overhead_stats->add(diagnostic_overhead_us);
            write_stats->add(write_us);
            cycle_stats->add(cycle_us);
            health_tracker.addBlock(render_us, producer_work_us);
            if (cycle_us > kBlockBudgetUs) {
                ++(*cycle_over_budget);
            }
            if (cycle_us > dma_window_us) {
                ++(*producer_stalls);
            }
            pcm_stats.add(mono_output);
            *checksum = updateChecksum(*checksum, mono_output);
        }
        return true;
    };

    std::uint32_t cycle_over_budget = 0;
    std::uint32_t producer_stalls = 0;
    std::uint32_t write_timeouts = 0;
    std::uint32_t partial_writes = 0;
    std::uint64_t checksum = 1469598103934665603ull;
    LatencyStats render_stats{};
    LatencyStats producer_work_stats{};
    LatencyStats diagnostic_overhead_stats{};
    LatencyStats write_stats{};
    LatencyStats cycle_stats{};

    const std::uint64_t clipped_before = context.renderer->stats().clipped_samples;

    for (std::size_t block = 0;
         block < static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_WARMUP_BLOCKS);
         ++block) {
        if (!renderAndWrite(false, &render_stats, &producer_work_stats,
                            &diagnostic_overhead_stats, &write_stats, &cycle_stats,
                            &cycle_over_budget, &producer_stalls, &write_timeouts,
                            &partial_writes, &checksum)) {
            cleanup();
            return false;
        }
    }

    const std::uint32_t synth_start_sent_callbacks = callback_stats.sent_events;
    const std::uint32_t synth_start_send_queue_overflows = callback_stats.send_queue_overflows;
    const std::uint32_t synth_warmup_sent_callbacks =
        synth_start_sent_callbacks - tone_end_sent_callbacks;
    const std::uint32_t synth_warmup_send_queue_overflows =
        synth_start_send_queue_overflows - tone_end_send_queue_overflows;

    for (std::size_t block = 0;
         block < static_cast<std::size_t>(CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS);
         ++block) {
        if (!renderAndWrite(true, &render_stats, &producer_work_stats,
                            &diagnostic_overhead_stats, &write_stats, &cycle_stats,
                            &cycle_over_budget, &producer_stalls, &write_timeouts,
                            &partial_writes, &checksum)) {
            cleanup();
            return false;
        }
    }

    const std::uint32_t active_end_sent_callbacks = callback_stats.sent_events;
    const std::uint32_t active_end_send_queue_overflows = callback_stats.send_queue_overflows;
    const std::uint32_t synth_sent_callbacks =
        active_end_sent_callbacks - synth_start_sent_callbacks;
    const std::uint32_t synth_send_queue_overflows =
        active_end_send_queue_overflows - synth_start_send_queue_overflows;
    const std::uint32_t active_synth_send_queue_overflows =
        active_end_send_queue_overflows - tone_end_send_queue_overflows;
    const std::uint64_t clipped_samples =
        context.renderer->stats().clipped_samples - clipped_before;
    const UBaseType_t stack_high_water_bytes = uxTaskGetStackHighWaterMark(nullptr);
    const InternalHeapSnapshot before_cleanup_heap = internalHeapSnapshot();
    const realtime::RealtimeHealthSnapshot health = health_tracker.snapshot();

    cleanup();

    const std::uint32_t final_sent_callbacks = callback_stats.sent_events;
    const std::uint32_t final_send_queue_overflows = callback_stats.send_queue_overflows;
    const std::uint32_t shutdown_tail_callbacks =
        final_sent_callbacks - active_end_sent_callbacks;
    const std::uint32_t shutdown_tail_overflows =
        final_send_queue_overflows - active_end_send_queue_overflows;
    const InternalHeapSnapshot after_cleanup_heap = internalHeapSnapshot();

    logInternalHeap("before_cleanup", before_cleanup_heap);
    logInternalHeap("after_cleanup", after_cleanup_heap);

    ESP_LOGI(
        kTag,
        "i2s_qual: PATH startup_prime_blocks=%u startup_sent_callbacks=%u startup_prime_send_queue_overflows=%u startup_overflow_gate=diagnostic_only tone_hz=%u tone_blocks=%u tone_peak=%d tone_sent_callbacks=%u tone_send_queue_overflows=%u tone_checksum=%016llx synth_warmup_callbacks=%u synth_warmup_send_queue_overflows=%u synth_sent_callbacks=%u synth_send_queue_overflows=%u active_synth_send_queue_overflows=%u shutdown_tail_callbacks=%u shutdown_tail_overflows=%u",
        static_cast<unsigned>(live_prime_blocks),
        static_cast<unsigned>(startup_sent_callbacks),
        static_cast<unsigned>(startup_send_queue_overflows),
        static_cast<unsigned>(kReferenceToneHz),
        static_cast<unsigned>(kReferenceToneBlocks),
        static_cast<int>(kReferenceTonePeak),
        static_cast<unsigned>(tone_sent_callbacks),
        static_cast<unsigned>(tone_send_queue_overflows),
        static_cast<unsigned long long>(tone_checksum),
        static_cast<unsigned>(synth_warmup_sent_callbacks),
        static_cast<unsigned>(synth_warmup_send_queue_overflows),
        static_cast<unsigned>(synth_sent_callbacks),
        static_cast<unsigned>(synth_send_queue_overflows),
        static_cast<unsigned>(active_synth_send_queue_overflows),
        static_cast<unsigned>(shutdown_tail_callbacks),
        static_cast<unsigned>(shutdown_tail_overflows));

    ESP_LOGI(
        kTag,
        "i2s_qual: RESULT blocks=%u render_us[min/avg/max]=%u/%u/%u producer_work_us[min/avg/max]=%u/%u/%u diagnostic_overhead_us[min/avg/max]=%u/%u/%u write_wait_us[min/avg/max]=%u/%u/%u cycle_us[min/avg/max]=%u/%u/%u render_deadline_misses=%u render_deadline_gate=soft cycle_over_budget=%u cycle_budget_gate=soft producer_stalls_over_dma_window=%u write_timeouts=%u partial_writes=%u pcm_peak_abs=%u pcm_avg_abs=%u pcm_nonzero_samples=%llu pcm_samples=%llu clipped_samples=%llu stack_high_water_bytes=%u checksum=%016llx",
        static_cast<unsigned>(CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS),
        static_cast<unsigned>(render_stats.minimum()),
        static_cast<unsigned>(render_stats.average()),
        static_cast<unsigned>(render_stats.max_us),
        static_cast<unsigned>(producer_work_stats.minimum()),
        static_cast<unsigned>(producer_work_stats.average()),
        static_cast<unsigned>(producer_work_stats.max_us),
        static_cast<unsigned>(diagnostic_overhead_stats.minimum()),
        static_cast<unsigned>(diagnostic_overhead_stats.average()),
        static_cast<unsigned>(diagnostic_overhead_stats.max_us),
        static_cast<unsigned>(write_stats.minimum()),
        static_cast<unsigned>(write_stats.average()),
        static_cast<unsigned>(write_stats.max_us),
        static_cast<unsigned>(cycle_stats.minimum()),
        static_cast<unsigned>(cycle_stats.average()),
        static_cast<unsigned>(cycle_stats.max_us),
        static_cast<unsigned>(health.render_soft_overruns),
        static_cast<unsigned>(cycle_over_budget),
        static_cast<unsigned>(producer_stalls),
        static_cast<unsigned>(write_timeouts),
        static_cast<unsigned>(partial_writes),
        static_cast<unsigned>(pcm_stats.peak_abs),
        static_cast<unsigned>(pcm_stats.averageAbs()),
        static_cast<unsigned long long>(pcm_stats.nonzero_samples),
        static_cast<unsigned long long>(pcm_stats.samples),
        static_cast<unsigned long long>(clipped_samples),
        static_cast<unsigned>(stack_high_water_bytes),
        static_cast<unsigned long long>(checksum));

    ESP_LOGI(
        kTag,
        "i2s_qual: REALTIME_HEALTH block_budget_us=%u dma_window_us=%u render_soft_overruns=%u producer_work_soft_overruns=%u avg_producer_work_us=%u max_consecutive_work_overruns=%u max_backlog_us=%llu max_backlog_blocks=%u max_backlog_recovery_blocks=%u final_backlog_us=%llu min_dma_margin_us=%u backlog_exceeded_dma_window=%u backlog_healthy=%u",
        static_cast<unsigned>(health.block_budget_us),
        static_cast<unsigned>(health.dma_window_us),
        static_cast<unsigned>(health.render_soft_overruns),
        static_cast<unsigned>(health.producer_work_soft_overruns),
        static_cast<unsigned>(health.averageProducerWorkUs()),
        static_cast<unsigned>(health.max_consecutive_work_overruns),
        static_cast<unsigned long long>(health.max_backlog_us),
        static_cast<unsigned>(health.max_backlog_blocks),
        static_cast<unsigned>(health.max_backlog_recovery_blocks),
        static_cast<unsigned long long>(health.final_backlog_us),
        static_cast<unsigned>(health.min_dma_margin_us),
        health.backlog_exceeded_dma_window ? 1u : 0u,
        health.backlogHealthy() ? 1u : 0u);

    constexpr std::uint32_t kFailProducerStall = 1u << 0u;
    constexpr std::uint32_t kFailWriteTimeout = 1u << 1u;
    constexpr std::uint32_t kFailPartialWrite = 1u << 2u;
    constexpr std::uint32_t kFailToneQueueOverflow = 1u << 3u;
    constexpr std::uint32_t kFailSynthQueueOverflow = 1u << 4u;
    constexpr std::uint32_t kFailBacklog = 1u << 5u;
    constexpr std::uint32_t kFailSilentOutput = 1u << 6u;
    constexpr std::uint32_t kFailClipping = 1u << 7u;
    constexpr std::uint32_t kFailStackHeadroom = 1u << 8u;

    std::uint32_t hard_failure_flags = 0u;
    if (producer_stalls != 0u) {
        hard_failure_flags |= kFailProducerStall;
    }
    if (write_timeouts != 0u) {
        hard_failure_flags |= kFailWriteTimeout;
    }
    if (partial_writes != 0u) {
        hard_failure_flags |= kFailPartialWrite;
    }
    if (tone_send_queue_overflows != 0u) {
        hard_failure_flags |= kFailToneQueueOverflow;
    }
    if (active_synth_send_queue_overflows != 0u) {
        hard_failure_flags |= kFailSynthQueueOverflow;
    }
    if (!health.backlogHealthy()) {
        hard_failure_flags |= kFailBacklog;
    }
    if (pcm_stats.peak_abs == 0u || pcm_stats.nonzero_samples == 0u) {
        hard_failure_flags |= kFailSilentOutput;
    }
    if (clipped_samples != 0u) {
        hard_failure_flags |= kFailClipping;
    }
    if (stack_high_water_bytes < CONFIG_GM_SYNTH_I2S_QUAL_MIN_STACK_HEADROOM_BYTES) {
        hard_failure_flags |= kFailStackHeadroom;
    }

    ESP_LOGI(
        kTag,
        "i2s_qual: HARD_GATE failure_flags=0x%08x producer_stalls=%u write_timeouts=%u partial_writes=%u tone_queue_overflows=%u active_synth_queue_overflows=%u backlog_healthy=%u clipped_samples=%llu stack_headroom_ok=%u transient_render_overruns=%u transient_cycle_overruns=%u",
        static_cast<unsigned>(hard_failure_flags),
        static_cast<unsigned>(producer_stalls),
        static_cast<unsigned>(write_timeouts),
        static_cast<unsigned>(partial_writes),
        static_cast<unsigned>(tone_send_queue_overflows),
        static_cast<unsigned>(active_synth_send_queue_overflows),
        health.backlogHealthy() ? 1u : 0u,
        static_cast<unsigned long long>(clipped_samples),
        stack_high_water_bytes >= CONFIG_GM_SYNTH_I2S_QUAL_MIN_STACK_HEADROOM_BYTES ? 1u : 0u,
        static_cast<unsigned>(health.render_soft_overruns),
        static_cast<unsigned>(cycle_over_budget));

    const bool pass = hard_failure_flags == 0u;

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
    vTaskDelay(1);
    return context.success;
}

}  // namespace gm_synth::bringup
