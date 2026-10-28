#pragma once

#include "KorvoV4Es8311.h"
#include "driver/i2s_std.h"
#include <atomic>
#include <cstdint>

namespace gm_synth::bringup {

struct LoopRealtimeI2sStats {
    std::uint32_t writes = 0;
    std::uint32_t write_errors = 0;
    std::uint32_t timeouts = 0;
    std::uint32_t partial_writes = 0;
    std::uint32_t sent = 0;
    std::uint32_t queue_overflows = 0;
};

// Dedicated Korvo qualification sink. All methods belong to the audio task;
// construction/destruction may instead occur before start/after that task joins.
// start/stop are outside measured playback. write performs only the bounded
// stereo->mono conversion and one DMA-paced write, with no allocation/logging.
class LoopRealtimeI2s {
public:
    static constexpr std::uint32_t kSampleRate = 44100;
    static constexpr std::uint32_t kFrames = 64;
    static constexpr std::uint32_t kDmaDescriptors = 8;
    static constexpr std::uint32_t kDmaWindowUs = 11610;
    LoopRealtimeI2s();
    ~LoopRealtimeI2s() { stop(); }
    LoopRealtimeI2s(const LoopRealtimeI2s&) = delete;
    LoopRealtimeI2s& operator=(const LoopRealtimeI2s&) = delete;

    bool start();
    bool write(const std::int16_t* stereo, std::uint32_t& downmix_us);
    void stop();
    LoopRealtimeI2sStats stats() const noexcept;
private:
    static bool onSent(i2s_chan_handle_t, i2s_event_data_t*, void*);
    static bool onOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*);
    bool writeSilence();
    KorvoV4Es8311 codec_;
    i2s_chan_handle_t tx_ = nullptr;
    bool enabled_ = false, measuring_ = false;
    std::atomic<std::uint32_t> sent_{0}, overflows_{0};
    std::uint32_t first_sent_ = 0, first_overflows_ = 0;
    LoopRealtimeI2sStats stats_{};
};

} // namespace gm_synth::bringup
