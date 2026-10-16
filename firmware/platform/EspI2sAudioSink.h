#pragma once

#include "driver/i2s_std.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::platform {

struct EspI2sAudioSinkConfig {
    std::uint32_t sample_rate_hz = 44100;
    int bclk_gpio = -1;
    int ws_gpio = -1;
    int dout_gpio = -1;
    int mclk_gpio = -1;
    std::uint32_t dma_desc_num = 6;
    std::uint32_t dma_frame_num = 64;
};

struct EspI2sAudioSinkStats {
    std::uint64_t write_calls = 0;
    std::uint64_t frames_submitted = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t short_writes = 0;
    std::uint64_t failures = 0;
};

// Control domain owns begin()/shutdown(). The audio task calls writeFrames()
// with already-rendered signed 16-bit interleaved stereo PCM. No SF2/storage
// or Voice ownership logic lives in this layer.
class EspI2sAudioSink {
public:
    EspI2sAudioSink() = default;
    ~EspI2sAudioSink();

    EspI2sAudioSink(const EspI2sAudioSink&) = delete;
    EspI2sAudioSink& operator=(const EspI2sAudioSink&) = delete;

    bool begin(const EspI2sAudioSinkConfig& config);
    bool writeFrames(
        const std::int16_t* interleaved_stereo,
        std::size_t frame_count,
        std::uint32_t timeout_ms,
        std::size_t* bytes_written = nullptr);
    void shutdown();

    bool ready() const { return tx_handle_ != nullptr && enabled_; }
    const EspI2sAudioSinkStats& stats() const { return stats_; }

private:
    static bool validConfig(const EspI2sAudioSinkConfig& config);

    i2s_chan_handle_t tx_handle_ = nullptr;
    bool enabled_ = false;
    EspI2sAudioSinkStats stats_{};
};

}  // namespace gm_synth::platform
