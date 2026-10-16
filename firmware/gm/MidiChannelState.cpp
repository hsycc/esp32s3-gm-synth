#include "MidiChannelState.h"

#include <algorithm>

namespace gm_synth::gm {

void MidiChannelState::resetControllers() {
    // MIDI CC121 / RP-015 style reset: preserve mix/program state such as
    // Channel Volume, Pan, Bank, Program and effect sends. Reset performance
    // controllers, Pitch Bend and parameter selection instead. A full GM System
    // On reset is a separate operation and will restore mix defaults too.
    expression = 127;
    sustain = 0;
    pitch_bend = kPitchBendCenter;
    rpn_msb = 127;
    rpn_lsb = 127;
    nrpn_msb = 127;
    nrpn_lsb = 127;
    nrpn_selected = false;
}

void MidiChannelState::resetAll() {
    bank_msb = 0;
    bank_lsb = 0;
    program = 0;
    volume = 100;
    pan = 64;
    reverb_send = 0;
    chorus_send = 0;
    pitch_bend_range_semitones = 2;
    pitch_bend_range_cents = 0;
    resetControllers();
}

std::uint32_t MidiChannelState::applyControlChange(
    std::uint8_t controller,
    std::uint8_t value) {
    if (controller > 127 || value > 127) {
        return ChannelControlNone;
    }

    switch (controller) {
        case 0:
            bank_msb = value;
            return ChannelControlBankChanged;
        case 32:
            bank_lsb = value;
            return ChannelControlBankChanged;
        case 7:
            volume = value;
            return ChannelControlGainChanged;
        case 10:
            pan = value;
            return ChannelControlPanChanged;
        case 11:
            expression = value;
            return ChannelControlGainChanged;
        case 64: {
            const bool before = sustainDown();
            sustain = value;
            return before == sustainDown() ? ChannelControlNone : ChannelControlSustainChanged;
        }
        case 91:
            reverb_send = value;
            return ChannelControlEffectsChanged;
        case 93:
            chorus_send = value;
            return ChannelControlEffectsChanged;
        case 98:
            nrpn_lsb = value;
            nrpn_selected = true;
            return ChannelControlRpnSelectionChanged;
        case 99:
            nrpn_msb = value;
            nrpn_selected = true;
            return ChannelControlRpnSelectionChanged;
        case 100:
            rpn_lsb = value;
            nrpn_selected = false;
            return ChannelControlRpnSelectionChanged;
        case 101:
            rpn_msb = value;
            nrpn_selected = false;
            return ChannelControlRpnSelectionChanged;
        case 6:
            if (pitchBendRpnSelected()) {
                pitch_bend_range_semitones = value;
                return ChannelControlPitchRangeChanged;
            }
            return ChannelControlNone;
        case 38:
            if (pitchBendRpnSelected()) {
                pitch_bend_range_cents = std::min<std::uint8_t>(value, 99);
                return ChannelControlPitchRangeChanged;
            }
            return ChannelControlNone;
        case 96:
            if (pitchBendRpnSelected() && pitch_bend_range_semitones < 127) {
                ++pitch_bend_range_semitones;
                return ChannelControlPitchRangeChanged;
            }
            return ChannelControlNone;
        case 97:
            if (pitchBendRpnSelected() && pitch_bend_range_semitones > 0) {
                --pitch_bend_range_semitones;
                return ChannelControlPitchRangeChanged;
            }
            return ChannelControlNone;
        case 120:
            return ChannelControlAllSoundOff;
        case 121:
            resetControllers();
            return ChannelControlResetControllers |
                ChannelControlGainChanged |
                ChannelControlSustainChanged |
                ChannelControlRpnSelectionChanged;
        case 123:
            return ChannelControlAllNotesOff;
        default:
            return ChannelControlNone;
    }
}

bool MidiChannelState::applyPitchBend(std::uint16_t value14) {
    if (value14 > 0x3fffu) {
        return false;
    }
    pitch_bend = value14;
    return true;
}

bool MidiChannelState::applyProgramChange(std::uint8_t new_program) {
    if (new_program > 127) {
        return false;
    }
    program = new_program;
    return true;
}

MidiChannelStateBank::MidiChannelStateBank() {
    resetAll();
}

std::uint32_t MidiChannelStateBank::applyControlChange(
    std::uint8_t channel_index,
    std::uint8_t controller,
    std::uint8_t value) {
    if (channel_index >= channels_.size()) {
        return ChannelControlNone;
    }
    return channels_[channel_index].applyControlChange(controller, value);
}

bool MidiChannelStateBank::applyPitchBend(
    std::uint8_t channel_index,
    std::uint16_t value14) {
    return channel_index < channels_.size() &&
        channels_[channel_index].applyPitchBend(value14);
}

bool MidiChannelStateBank::applyProgramChange(
    std::uint8_t channel_index,
    std::uint8_t program_value) {
    return channel_index < channels_.size() &&
        channels_[channel_index].applyProgramChange(program_value);
}

void MidiChannelStateBank::resetAll() {
    for (MidiChannelState& state : channels_) {
        state.resetAll();
    }
}

}  // namespace gm_synth::gm
