#include "LoopRealtimeI2s.h"
#include "util/RealtimeAtomicContract.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace gm_synth::bringup {
namespace {
bool withinOnePercent(std::uint32_t actual, std::uint32_t expected) {
    const std::uint32_t difference = actual > expected ? actual - expected : expected - actual;
    return std::uint64_t{difference} * 100u <= expected;
}
}

LoopRealtimeI2s::LoopRealtimeI2s() {
    util::requireRealtimeAtomic32(sent_);
    util::requireRealtimeAtomic32(overflows_);
}

bool IRAM_ATTR LoopRealtimeI2s::onSent(i2s_chan_handle_t, i2s_event_data_t*, void* opaque) {
    auto& self = *static_cast<LoopRealtimeI2s*>(opaque);
    // One ISR writer; the audio owner only reads this counter.
    self.sent_.store(self.sent_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    return false;
}
bool IRAM_ATTR LoopRealtimeI2s::onOverflow(i2s_chan_handle_t, i2s_event_data_t*, void* opaque) {
    auto& self = *static_cast<LoopRealtimeI2s*>(opaque);
    self.overflows_.store(self.overflows_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    return false;
}

bool LoopRealtimeI2s::writeSilence() {
    std::array<std::int16_t, kFrames> silence{};
    std::size_t written = 0;
    return i2s_channel_write(tx_, silence.data(), sizeof(silence), &written,
        CONFIG_GM_SYNTH_I2S_QUAL_WRITE_TIMEOUT_MS) == ESP_OK && written == sizeof(silence);
}

bool LoopRealtimeI2s::start() {
    if (tx_ || CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM != kDmaDescriptors ||
        CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM != kFrames ||
        CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15 != 32767 ||
        CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB != 12 ||
        CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO != 16 ||
        CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO != 9 ||
        CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO != 45 ||
        CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO != 8) return false;
    stats_ = {};
    sent_.store(0, std::memory_order_relaxed);
    overflows_.store(0, std::memory_order_relaxed);
    const auto fail = [this]() { stop(); return false; };
    if (!codec_.prepareMuted()) return fail();

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = kDmaDescriptors;
    channel.dma_frame_num = kFrames;
    if (i2s_new_channel(&channel, &tx_, nullptr) != ESP_OK) return fail();
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    standard.gpio_cfg.mclk = GPIO_NUM_16;
    standard.gpio_cfg.bclk = GPIO_NUM_9;
    standard.gpio_cfg.ws = GPIO_NUM_45;
    standard.gpio_cfg.dout = GPIO_NUM_8;
    standard.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx_, &standard) != ESP_OK) return fail();
    i2s_chan_info_t info{};
    if (i2s_channel_get_info(tx_, &info) != ESP_OK ||
        !withinOnePercent(info.bclk_hz, kSampleRate * 32u) ||
        !withinOnePercent(info.mclk_hz, kSampleRate * 256u)) return fail();
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = onSent;
    callbacks.on_send_q_ovf = onOverflow;
    if (i2s_channel_register_event_callback(tx_, &callbacks, this) != ESP_OK) return fail();
    std::array<std::int16_t, kFrames> silence{};
    std::size_t preloaded = 0;
    for (unsigned i = 0; i <= kDmaDescriptors; ++i) {
        std::size_t loaded = 0;
        if (i2s_channel_preload_data(tx_, silence.data(), sizeof(silence), &loaded) != ESP_OK) return fail();
        preloaded += loaded;
        if (loaded != sizeof(silence)) break;
    }
    if (!info.total_dma_buf_size || preloaded != info.total_dma_buf_size) return fail();
    std::printf("REALTIME_I2S_ARMED sample_rate=44100 frames=64 dma_desc=8 dma_frames=64 dma_window_us=11610 master_q15=32767 dac_db=-12 mono_avg_lr=1 bclk=%u mclk=%u dma_bytes=%u\n",
        unsigned(info.bclk_hz), unsigned(info.mclk_hz), unsigned(info.total_dma_buf_size));
    if (i2s_channel_enable(tx_) != ESP_OK) return fail();
    enabled_ = true;
    if (!codec_.startMutedWithClocks()) return fail();
    for (unsigned i = 0; i < 2u * kDmaDescriptors; ++i) if (!writeSilence()) return fail();
    if (!codec_.enableOutput()) return fail();
    // No logging after PA enable; the first renderer block follows immediately.
    first_sent_ = sent_.load(std::memory_order_relaxed);
    first_overflows_ = overflows_.load(std::memory_order_relaxed);
    measuring_ = true;
    return true;
}

bool LoopRealtimeI2s::write(const std::int16_t* stereo, std::uint32_t& downmix_us) {
    downmix_us = 0;
    if (!enabled_ || !stereo) return false;
    const auto before = esp_timer_get_time();
    std::array<std::int16_t, kFrames> mono{};
    for (std::size_t i = 0; i < kFrames; ++i)
        mono[i] = static_cast<std::int16_t>((std::int32_t{stereo[2u * i]} + stereo[2u * i + 1u]) / 2);
    downmix_us = static_cast<std::uint32_t>(esp_timer_get_time() - before);
    std::size_t written = 0;
    const auto error = i2s_channel_write(tx_, mono.data(), sizeof(mono), &written,
        CONFIG_GM_SYNTH_I2S_QUAL_WRITE_TIMEOUT_MS);
    ++stats_.writes;
    if (error != ESP_OK) ++stats_.write_errors;
    if (error == ESP_ERR_TIMEOUT) ++stats_.timeouts;
    if (written != sizeof(mono)) ++stats_.partial_writes;
    return error == ESP_OK && written == sizeof(mono);
}

LoopRealtimeI2sStats LoopRealtimeI2s::stats() const noexcept {
    auto result = stats_;
    if (measuring_) {
        result.sent = sent_.load(std::memory_order_relaxed) - first_sent_;
        result.queue_overflows = overflows_.load(std::memory_order_relaxed) - first_overflows_;
    }
    return result;
}

void LoopRealtimeI2s::stop() {
    if (measuring_) { stats_ = stats(); measuring_ = false; }
    codec_.shutdown(); // PA off before clocks/DMA teardown, including failures.
    if (tx_) {
        if (enabled_ && i2s_channel_disable(tx_) != ESP_OK) {
            // The callback still owns this object until the driver is stopped.
            // Do not return/free its task stack with a potentially live ISR.
            std::puts("REALTIME_FAIL i2s_disable resources_retained=1");
            std::fflush(stdout); std::abort();
        }
        if (i2s_del_channel(tx_) != ESP_OK) {
            std::puts("REALTIME_FAIL i2s_delete resources_retained=1");
            std::fflush(stdout); std::abort();
        }
        tx_ = nullptr;
    }
    enabled_ = false;
}

} // namespace gm_synth::bringup
