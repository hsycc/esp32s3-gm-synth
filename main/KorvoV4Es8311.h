#pragma once

#include "driver/i2c_master.h"

namespace gm_synth::bringup {

class KorvoV4Es8311 {
public:
    KorvoV4Es8311() = default;
    ~KorvoV4Es8311();

    KorvoV4Es8311(const KorvoV4Es8311&) = delete;
    KorvoV4Es8311& operator=(const KorvoV4Es8311&) = delete;

    // Keep PA disabled, configure the ES8311 control plane, and hold the DAC
    // digitally muted while the I2S DMA ring is configured/preloaded.
    bool prepareMuted();

    // Call only after the I2S channel is enabled and MCLK/BCLK/LRCK are live.
    // This performs the ES8311 DAC power-up/start sequence while remaining muted.
    bool startMutedWithClocks();

    // Final anti-pop startup step after the live DMA queue has been stabilized.
    bool enableOutput();

    // Anti-pop shutdown: PA off first, then mute/power-down codec, then free I2C.
    void shutdown();

private:
    bool writeReg(unsigned reg, unsigned value);
    bool readReg(unsigned reg, unsigned& value);
    bool writeSequence(const unsigned char (*sequence)[2], unsigned count);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    bool owns_bus_ = false;
    bool prepared_ = false;
    bool started_ = false;
    bool output_enabled_ = false;
};

}  // namespace gm_synth::bringup
