#include "KorvoV4Es8311.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr gpio_num_t kSda = GPIO_NUM_17;
constexpr gpio_num_t kScl = GPIO_NUM_18;
constexpr gpio_num_t kPaEnable = GPIO_NUM_48;
constexpr std::uint16_t kEs8311Address = 0x18;
constexpr std::uint32_t kI2cHz = 400000;
constexpr int kI2cTimeoutMs = 100;

constexpr unsigned kRegReset = 0x00;
constexpr unsigned kRegClock1 = 0x01;
constexpr unsigned kRegClock2 = 0x02;
constexpr unsigned kRegClock3 = 0x03;
constexpr unsigned kRegClock4 = 0x04;
constexpr unsigned kRegClock5 = 0x05;
constexpr unsigned kRegClock6 = 0x06;
constexpr unsigned kRegClock7 = 0x07;
constexpr unsigned kRegClock8 = 0x08;
constexpr unsigned kRegSdpIn = 0x09;
constexpr unsigned kRegSdpOut = 0x0A;
constexpr unsigned kRegSystem0B = 0x0B;
constexpr unsigned kRegSystem0C = 0x0C;
constexpr unsigned kRegSystem0D = 0x0D;
constexpr unsigned kRegSystem0E = 0x0E;
constexpr unsigned kRegSystem10 = 0x10;
constexpr unsigned kRegSystem11 = 0x11;
constexpr unsigned kRegSystem12 = 0x12;
constexpr unsigned kRegSystem13 = 0x13;
constexpr unsigned kRegSystem14 = 0x14;
constexpr unsigned kRegAdc15 = 0x15;
constexpr unsigned kRegAdc16 = 0x16;
constexpr unsigned kRegAdc17 = 0x17;
constexpr unsigned kRegAdc1B = 0x1B;
constexpr unsigned kRegAdc1C = 0x1C;
constexpr unsigned kRegDacMute = 0x31;
constexpr unsigned kRegDacVolume = 0x32;
constexpr unsigned kRegDacRamp = 0x37;
constexpr unsigned kRegGpio44 = 0x44;
constexpr unsigned kRegGp45 = 0x45;
constexpr unsigned kRegChipId1 = 0xFD;
constexpr unsigned kRegChipId2 = 0xFE;

// ES8311 volume is 0.5 dB/LSB over -95.5..+32 dB in Espressif's
// reference driver. 0x8F therefore selects -24 dB for safe first audio.
constexpr unsigned kStartupVolumeMinus24Db = 0x8F;

// Fixed DAC-only setup derived from Espressif's ES8311 reference driver for:
// slave mode, external MCLK, normal Philips I2S, 16-bit samples,
// Fs=44.1 kHz and MCLK=11.2896 MHz (256 * Fs).
constexpr unsigned char kOpenSequence[][2] = {
    {kRegGpio44, 0x08},
    {kRegGpio44, 0x08},  // reference driver intentionally repeats first write
    {kRegClock1, 0x30},
    {kRegClock2, 0x00},
    {kRegClock3, 0x10},
    {kRegAdc16, 0x24},
    {kRegClock4, 0x10},
    {kRegClock5, 0x00},
    {kRegSystem0B, 0x00},
    {kRegSystem0C, 0x00},
    {kRegSystem10, 0x1F},
    {kRegSystem11, 0x7F},
    {kRegReset, 0x80},        // codec slave mode
    {kRegClock1, 0x3F},       // external MCLK, non-inverted
    {kRegClock6, 0x00},       // non-inverted SCLK before rate programming
    {kRegSystem13, 0x10},
    {kRegAdc1B, 0x0A},
    {kRegAdc1C, 0x6A},
    {kRegGpio44, 0x58},       // internal DAC reference
    {kRegSdpIn, 0x0C},        // normal I2S, 16-bit DAC input
    {kRegSdpOut, 0x0C},
    {kRegClock2, 0x00},       // 44.1k / 11.2896M: pre-div=1, mult=1
    {kRegClock5, 0x00},       // ADC/DAC clock dividers = 1
    {kRegClock3, 0x10},       // fs_mode=0, ADC OSR=0x10
    {kRegClock4, 0x10},       // DAC OSR=0x10
    {kRegClock7, 0x00},
    {kRegClock8, 0xFF},       // LRCK divider low byte
    {kRegClock6, 0x03},       // BCLK divider 4
};

constexpr unsigned char kStartDacSequence[][2] = {
    {kRegReset, 0x80},
    {kRegClock1, 0x3F},
    {kRegSdpIn, 0x0C},
    {kRegSdpOut, 0x0C},
    {kRegAdc17, 0xBF},
    {kRegSystem0E, 0x02},
    {kRegSystem12, 0x00},     // enable DAC
    {kRegSystem14, 0x1A},
    {kRegSystem0D, 0x01},
    {kRegAdc15, 0x40},
    {kRegDacRamp, 0x08},
    {kRegGp45, 0x00},
};

constexpr unsigned char kSuspendSequence[][2] = {
    {kRegDacVolume, 0x00},
    {kRegAdc17, 0x00},
    {kRegSystem0E, 0xFF},
    {kRegSystem12, 0x02},
    {kRegSystem14, 0x00},
    {kRegSystem0D, 0xFA},
    {kRegAdc15, 0x00},
    {kRegClock2, 0x10},
    {kRegReset, 0x00},
    {kRegReset, 0x1F},
    {kRegClock1, 0x30},
    {kRegClock1, 0x00},
    {kRegGp45, 0x00},
    {kRegSystem0D, 0xFC},
    {kRegClock2, 0x00},
};

}  // namespace

KorvoV4Es8311::~KorvoV4Es8311() {
    shutdown();
}

bool KorvoV4Es8311::writeReg(unsigned reg, unsigned value) {
    if (device_ == nullptr) {
        return false;
    }
    const std::uint8_t bytes[2] = {
        static_cast<std::uint8_t>(reg),
        static_cast<std::uint8_t>(value),
    };
    const esp_err_t err =
        i2c_master_transmit(device_, bytes, sizeof(bytes), kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "es8311: write reg 0x%02x failed: %s",
                 reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool KorvoV4Es8311::readReg(unsigned reg, unsigned& value) {
    if (device_ == nullptr) {
        return false;
    }
    const std::uint8_t address = static_cast<std::uint8_t>(reg);
    std::uint8_t data = 0;
    const esp_err_t err = i2c_master_transmit_receive(
        device_, &address, 1, &data, 1, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "es8311: read reg 0x%02x failed: %s",
                 reg, esp_err_to_name(err));
        return false;
    }
    value = data;
    return true;
}

bool KorvoV4Es8311::writeSequence(
    const unsigned char (*sequence)[2],
    unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (!writeReg(sequence[i][0], sequence[i][1])) {
            return false;
        }
    }
    return true;
}

bool KorvoV4Es8311::prepareMuted() {
    shutdown();

    gpio_reset_pin(kPaEnable);
    if (gpio_set_direction(kPaEnable, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(kPaEnable, 0) != ESP_OK) {
        ESP_LOGE(kTag, "es8311: failed to force PA off");
        return false;
    }

    // Reuse I2C0 when another board service already owns it; otherwise create
    // the Korvo audio-control bus ourselves and remember ownership for cleanup.
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &bus_);
    if (err == ESP_OK && bus_ != nullptr) {
        owns_bus_ = false;
    } else {
        i2c_master_bus_config_t bus_config{};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = kSda;
        bus_config.scl_io_num = kScl;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;
        err = i2c_new_master_bus(&bus_config, &bus_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "es8311: create I2C0 failed: %s", esp_err_to_name(err));
            bus_ = nullptr;
            return false;
        }
        owns_bus_ = true;
    }

    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kEs8311Address;
    device_config.scl_speed_hz = kI2cHz;
    err = i2c_master_bus_add_device(bus_, &device_config, &device_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "es8311: add I2C device failed: %s", esp_err_to_name(err));
        shutdown();
        return false;
    }

    if (!writeSequence(kOpenSequence,
                       static_cast<unsigned>(sizeof(kOpenSequence) / sizeof(kOpenSequence[0]))) ||
        !writeSequence(kStartDacSequence,
                       static_cast<unsigned>(sizeof(kStartDacSequence) / sizeof(kStartDacSequence[0])))) {
        shutdown();
        return false;
    }

    unsigned mute_reg = 0;
    if (!readReg(kRegDacMute, mute_reg) ||
        !writeReg(kRegDacMute, (mute_reg & 0x9F) | 0x60) ||
        !writeReg(kRegDacVolume, kStartupVolumeMinus24Db)) {
        shutdown();
        return false;
    }

    unsigned chip_id1 = 0;
    unsigned chip_id2 = 0;
    if (!readReg(kRegChipId1, chip_id1) || !readReg(kRegChipId2, chip_id2)) {
        shutdown();
        return false;
    }

    prepared_ = true;
    ESP_LOGI(kTag,
             "es8311: PREPARED slave=1 fs=44100 mclk=11289600 format=i2s bits=16 volume_db=-24 pa_gpio=%d chip_id=%02x%02x",
             static_cast<int>(kPaEnable), chip_id1, chip_id2);
    return true;
}

bool KorvoV4Es8311::enableOutput() {
    if (!prepared_ || device_ == nullptr) {
        return false;
    }

    unsigned mute_reg = 0;
    if (!readReg(kRegDacMute, mute_reg) ||
        !writeReg(kRegDacMute, mute_reg & 0x9F)) {
        return false;
    }
    if (gpio_set_level(kPaEnable, 1) != ESP_OK) {
        (void)writeReg(kRegDacMute, (mute_reg & 0x9F) | 0x60);
        return false;
    }
    output_enabled_ = true;
    ESP_LOGI(kTag, "es8311: OUTPUT_ENABLED codec_unmuted=1 pa=1");
    return true;
}

void KorvoV4Es8311::shutdown() {
    // The PA is cut first even if I2C has become unavailable.
    (void)gpio_set_level(kPaEnable, 0);
    output_enabled_ = false;

    if (device_ != nullptr) {
        unsigned mute_reg = 0;
        if (readReg(kRegDacMute, mute_reg)) {
            (void)writeReg(kRegDacMute, (mute_reg & 0x9F) | 0x60);
        }
        (void)writeSequence(kSuspendSequence,
                            static_cast<unsigned>(sizeof(kSuspendSequence) / sizeof(kSuspendSequence[0])));
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }

    if (bus_ != nullptr && owns_bus_) {
        (void)i2c_del_master_bus(bus_);
    }
    bus_ = nullptr;
    owns_bus_ = false;
    prepared_ = false;
}

}  // namespace gm_synth::bringup
