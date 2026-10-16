#include "gm/MidiChannelState.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::ChannelControlBankChanged;
using gm_synth::gm::ChannelControlGainChanged;
using gm_synth::gm::ChannelControlPanChanged;
using gm_synth::gm::ChannelControlPitchRangeChanged;
using gm_synth::gm::ChannelControlRpnSelectionChanged;
using gm_synth::gm::ChannelControlSustainChanged;
using gm_synth::gm::MidiChannelState;
using gm_synth::gm::MidiChannelStateBank;

void testGmDefaults() {
    MidiChannelState state{};
    state.resetAll();
    assert(state.selectedBank() == 0);
    assert(state.program == 0);
    assert(state.volume == 100);
    assert(state.pan == 64);
    assert(state.expression == 127);
    assert(state.sustain == 0);
    assert(!state.sustainDown());
    assert(state.reverb_send == 0);
    assert(state.chorus_send == 0);
    assert(state.pitch_bend == MidiChannelState::kPitchBendCenter);
    assert(state.pitch_bend_range_semitones == 2);
    assert(state.pitch_bend_range_cents == 0);
    assert(state.rpnNull());
}

void testBankVolumePanExpressionAndSustain() {
    MidiChannelState state{};
    state.resetAll();

    assert((state.applyControlChange(0, 3) & ChannelControlBankChanged) != 0);
    assert((state.applyControlChange(32, 5) & ChannelControlBankChanged) != 0);
    assert(state.selectedBank() == ((3u << 7u) | 5u));

    assert((state.applyControlChange(7, 88) & ChannelControlGainChanged) != 0);
    assert((state.applyControlChange(10, 32) & ChannelControlPanChanged) != 0);
    assert((state.applyControlChange(11, 99) & ChannelControlGainChanged) != 0);
    assert(state.volume == 88);
    assert(state.pan == 32);
    assert(state.expression == 99);

    assert((state.applyControlChange(64, 63) & ChannelControlSustainChanged) == 0);
    assert(!state.sustainDown());
    assert((state.applyControlChange(64, 64) & ChannelControlSustainChanged) != 0);
    assert(state.sustainDown());
    assert((state.applyControlChange(64, 127) & ChannelControlSustainChanged) == 0);
    assert(state.sustainDown());
    assert((state.applyControlChange(64, 0) & ChannelControlSustainChanged) != 0);
    assert(!state.sustainDown());
}

void testPitchBendSensitivityRpn() {
    MidiChannelState state{};
    state.resetAll();

    // Data Entry is ignored until RPN 0,0 is selected.
    assert(state.applyControlChange(6, 12) == 0);
    assert(state.pitch_bend_range_semitones == 2);

    assert((state.applyControlChange(101, 0) & ChannelControlRpnSelectionChanged) != 0);
    assert((state.applyControlChange(100, 0) & ChannelControlRpnSelectionChanged) != 0);
    assert(state.pitchBendRpnSelected());

    assert((state.applyControlChange(6, 12) & ChannelControlPitchRangeChanged) != 0);
    assert((state.applyControlChange(38, 50) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 12);
    assert(state.pitch_bend_range_cents == 50);

    // Cents are bounded to the meaningful 0..99 range.
    assert((state.applyControlChange(38, 127) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_cents == 99);

    assert((state.applyControlChange(96, 0) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 13);
    assert((state.applyControlChange(97, 0) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 12);

    // RPN null disables Data Entry until a new parameter is selected.
    state.applyControlChange(101, 127);
    state.applyControlChange(100, 127);
    assert(state.rpnNull());
    assert(state.applyControlChange(6, 3) == 0);
    assert(state.pitch_bend_range_semitones == 12);

    // NRPN selection also prevents RPN data entry from mutating bend range.
    state.applyControlChange(99, 1);
    state.applyControlChange(98, 2);
    assert(state.applyControlChange(6, 7) == 0);
    assert(state.pitch_bend_range_semitones == 12);
}

void testPitchBendAndProgramValueValidation() {
    MidiChannelState state{};
    state.resetAll();
    assert(state.applyPitchBend(0));
    assert(state.pitch_bend == 0);
    assert(state.applyPitchBend(8192));
    assert(state.pitch_bend == 8192);
    assert(state.applyPitchBend(16383));
    assert(state.pitch_bend == 16383);
    assert(!state.applyPitchBend(16384));
    assert(state.pitch_bend == 16383);

    assert(state.applyProgramChange(127));
    assert(state.program == 127);
    assert(!state.applyProgramChange(255));
    assert(state.program == 127);
}

void testChannelBankIsIndependentAcross16Channels() {
    MidiChannelStateBank bank;
    assert(bank.channel(0).volume == 100);
    assert(bank.channel(15).volume == 100);
    assert((bank.applyControlChange(2, 7, 40) & ChannelControlGainChanged) != 0);
    assert(bank.channel(2).volume == 40);
    assert(bank.channel(1).volume == 100);
    assert(bank.applyControlChange(16, 7, 10) == 0);
    assert(!bank.applyPitchBend(16, 8192));
    assert(!bank.applyProgramChange(16, 1));
}

}  // namespace

int main() {
    testGmDefaults();
    testBankVolumePanExpressionAndSustain();
    testPitchBendSensitivityRpn();
    testPitchBendAndProgramValueValidation();
    testChannelBankIsIndependentAcross16Channels();
    std::cout << "GM MIDI channel-state host checks passed\n";
    return 0;
}
