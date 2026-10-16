#include "EspI2sAudioSink.h"

#include "esp_err.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::platform {
namespace {

void saturatingIncrement(std::uint64_t& value) {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

void saturatingAdd(std::uint64_t& value, std::uint64_t increment) {
    const std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max() - value;
    value += increment > remaining ? remaining : increment;
}

}  // namespace

EspI2sAudioSink::~EspI2sAudioSink() {
    shutdown();
}

bool EspI2sAudioSink::validConfig(const EspI2sAudioSinkConfig& config) {
    return config.sample_rate_hz != 0 &&
        config.bclk_gpio >= 0 &&
        config.ws_gpio >= 0 &&
        config.dout_gpio >= 0 &&
        config.mclk_gpio >= -1 &&
        config.dma_desc_num >= 2 &&
        config.dma_frame_num != 0 &&
        config.dma_frame_num <= 511;
}

bool EspI2sAudioSink::begin(const EspI2sAudioSinkConfig& config) {
    if (tx_handle_ != nullptr || !validConfig(config)) {
        saturatingIncrement(stats_.failures);
        return false;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = config.dma_desc_num;
    channel_config.dma_frame_num = config.dma_frame_num;

    if (i2s_new_channel(&channel_config, &tx_handle_, nullptr) != ESP_OK) {
        tx_handle_ = nullptr;
        saturatingIncrement(stats_.failures);
        return false;
    }

    const gpio_num_t mclk = config.mclk_gpio >= 0
        ? static_cast<gpio_num_t>(config.mclk_gpio)
        : I2S_GPIO_UNUSED;

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = static_cast<gpio_num_t>(config.bclk_gpio),
            .ws = static_cast<gpio_num_t>(config.ws_gpio),
            .dout = static_cast<gpio_num_t>(config.dout_gpio),
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (i2s_channel_init_std_mode(tx_handle_, &standard_config) != ESP_OK) {
        (void)i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        saturatingIncrement(stats_.failures);
        return false;
    }

    if (i2s_channel_enable(tx_handle_) != ESP_OK) {
        (void)i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        saturatingIncrement(stats_.failures);
        return false;
    }

    enabled_ = true;
    return true;
}

bool EspI2sAudioSink::writeFrames(
    const std::int16_t* interleaved_stereo,
    std::size_t frame_count,
    std::uint32_t timeout_ms,
    std::size_t* bytes_written) {
    if (bytes_written != nullptr) {
        *bytes_written = 0;
    }
    if (!ready() || interleaved_stereo == nullptr || frame_count == 0 ||
        frame_count > std::numeric_limits<std::size_t>::max() /
            (2u * sizeof(std::int16_t))) {
        saturatingIncrement(stats_.failures);
        return false;
    }

    const std::size_t requested_bytes =
        frame_count * 2u * sizeof(std::int16_t);
    std::size_t actual_bytes = 0;

    saturatingIncrement(stats_.write_calls);
    saturatingAdd(stats_.frames_submitted, static_cast<std::uint64_t>(frame_count));
    const esp_err_t result = i2s_channel_write(
        tx_handle_,
        interleaved_stereo,
        requested_bytes,
        &actual_bytes,
        timeout_ms);

    saturatingAdd(stats_.bytes_written, static_cast<std::uint64_t>(actual_bytes));
    if (bytes_written != nullptr) {
        *bytes_written = actual_bytes;
    }
    if (actual_bytes != requested_bytes) {
        saturatingIncrement(stats_.short_writes);
    }
    if (result == ESP_ERR_TIMEOUT) {
        saturatingIncrement(stats_.timeouts);
    }
    if (result != ESP_OK) {
        saturatingIncrement(stats_.failures);
        return false;
    }
    return actual_bytes == requested_bytes;
}

void EspI2sAudioSink::shutdown() {
    if (tx_handle_ == nullptr) {
        enabled_ = false;
        return;
    }

    if (enabled_) {
        (void)i2s_channel_disable(tx_handle_);
        enabled_ = false;
    }
    (void)i2s_del_channel(tx_handle_);
    tx_handle_ = nullptr;
}

}  // namespace gm_synth::platform
