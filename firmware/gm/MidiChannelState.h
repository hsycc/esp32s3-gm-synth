#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

// MIDI channels are zero-based internally; GM Channel 10 is therefore index 9.
// SoundFont reserves preset bank 128 for percussion even though ordinary MIDI
// Bank Select is a 14-bit controller pair.
constexpr std::uint8_t kGmPercussionChannel = 9;
constexpr std::uint16_t kSf2PercussionBank = 128;

enum ChannelControlAction : std::uint32_t {
    ChannelControlNone = 0,
    ChannelControlBankChanged = 1u << 0,
    ChannelControlGainChanged = 1u << 1,
    ChannelControlPanChanged = 1u << 2,
    ChannelControlSustainChanged = 1u << 3,
    ChannelControlEffectsChanged = 1u << 4,
    ChannelControlPitchRangeChanged = 1u << 5,
    ChannelControlRpnSelectionChanged = 1u << 6,
    ChannelControlAllSoundOff = 1u << 7,
    ChannelControlResetControllers = 1u << 8,
    ChannelControlAllNotesOff = 1u << 9,
    ChannelControlVibratoChanged = 1u << 10,
    ChannelControlFilterChanged = 1u << 11,
};

struct MidiChannelState {
    static constexpr std::uint16_t kPitchBendCenter = 8192;

    std::uint8_t bank_msb = 0;
    std::uint8_t bank_lsb = 0;
    std::uint8_t program = 0;

    std::uint8_t modulation_wheel = 0;
    // MIDI CC21 (SoundFont source operator 0x0195).
    std::uint8_t filter_controller_21 = 0;
    std::uint8_t channel_pressure = 0;
    std::uint8_t volume = 100;
    std::uint8_t pan = 64;
    std::uint8_t expression = 127;
    std::uint8_t sustain = 0;
    std::uint8_t reverb_send = 0;
    std::uint8_t chorus_send = 0;

    std::uint16_t pitch_bend = kPitchBendCenter;
    std::uint8_t pitch_bend_range_semitones = 2;
    std::uint8_t pitch_bend_range_cents = 0;

    std::uint8_t rpn_msb = 127;
    std::uint8_t rpn_lsb = 127;
    std::uint8_t nrpn_msb = 127;
    std::uint8_t nrpn_lsb = 127;
    bool nrpn_selected = false;

    std::uint16_t selectedBank() const {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bank_msb) << 7u) | bank_lsb);
    }

    bool sustainDown() const { return sustain >= 64; }
    bool pitchBendRpnSelected() const {
        return !nrpn_selected && rpn_msb == 0 && rpn_lsb == 0;
    }
    bool rpnNull() const {
        return !nrpn_selected && rpn_msb == 127 && rpn_lsb == 127;
    }

    void resetControllers();
    void resetAll();

    // Returns a ChannelControlAction bitmask. Unknown ordinary CC messages are
    // accepted as no-ops so a MIDI stream does not become invalid merely
    // because the current GM runtime has no destination for that controller.
    std::uint32_t applyControlChange(std::uint8_t controller, std::uint8_t value);
    bool applyChannelPressure(std::uint8_t pressure);
    bool applyPitchBend(std::uint16_t value14);
    bool applyProgramChange(std::uint8_t new_program);
};

class MidiChannelStateBank {
public:
    static constexpr std::size_t kMidiChannels = 16;

    MidiChannelStateBank();

    MidiChannelState& channel(std::uint8_t index) { return channels_[index]; }
    const MidiChannelState& channel(std::uint8_t index) const { return channels_[index]; }

    std::uint32_t applyControlChange(
        std::uint8_t channel,
        std::uint8_t controller,
        std::uint8_t value);
    bool applyChannelPressure(std::uint8_t channel, std::uint8_t pressure);
    bool applyPitchBend(std::uint8_t channel, std::uint16_t value14);
    bool applyProgramChange(std::uint8_t channel, std::uint8_t program);

    // Generic zero-bank reset retained for non-GM callers/tests.
    void resetAll();
    // GM System On state: melodic channels -> bank/program 0, Channel 10 ->
    // SoundFont percussion bank 128/program 0, with standard controller defaults.
    void resetGm();

    // Bank identity to use for a GM Program Change. In GM1 Channel 10 remains
    // key-based percussion even if Bank Select CCs arrive, so the SF2 lookup is
    // forced to the reserved percussion bank.
    std::uint16_t programBank(std::uint8_t channel_index) const;

private:
    std::array<MidiChannelState, kMidiChannels> channels_{};
};

}  // namespace gm_synth::gm
