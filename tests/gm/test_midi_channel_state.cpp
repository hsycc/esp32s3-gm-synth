#include "gm/MidiChannelState.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::ChannelControlBankChanged;
using gm_synth::gm::ChannelControlGainChanged;
using gm_synth::gm::ChannelControlFilterChanged;
using gm_synth::gm::ChannelControlPanChanged;
using gm_synth::gm::ChannelControlPitchRangeChanged;
using gm_synth::gm::ChannelControlResetControllers;
using gm_synth::gm::ChannelControlRpnSelectionChanged;
using gm_synth::gm::ChannelControlSustainChanged;
using gm_synth::gm::ChannelControlVibratoChanged;
using gm_synth::gm::MidiChannelState;
using gm_synth::gm::MidiChannelStateBank;

void testGmDefaults() {
    MidiChannelState state{};
    state.resetAll();
    assert(state.selectedBank() == 0);
    assert(state.program == 0);
    assert(state.modulation_wheel == 0);
    assert(state.channel_pressure == 0);
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

void testBankVolumePanExpressionSustainAndVibratoControllers() {
    MidiChannelState state{};
    state.resetAll();

    assert((state.applyControlChange(0, 3) & ChannelControlBankChanged) != 0);
    assert((state.applyControlChange(32, 5) & ChannelControlBankChanged) != 0);
    assert(state.selectedBank() == ((3u << 7u) | 5u));

    assert((state.applyControlChange(1, 80) & ChannelControlVibratoChanged) != 0);
    assert(state.modulation_wheel == 80);
    assert(state.applyChannelPressure(90));
    assert(state.channel_pressure == 90);
    assert(!state.applyChannelPressure(128));
    assert(state.channel_pressure == 90);

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

void testCc21FilterControllerLifecycle() {
    MidiChannelState state{};
    state.resetAll();
    assert(state.filter_controller_21 == 0);
    assert((state.applyControlChange(21, 64) & ChannelControlFilterChanged) != 0);
    assert(state.filter_controller_21 == 64);
    assert(state.applyControlChange(21, 64) == 0);
    assert((state.applyControlChange(21, 127) & ChannelControlFilterChanged) != 0);
    assert(state.filter_controller_21 == 127);
    assert((state.applyControlChange(121, 0) & ChannelControlFilterChanged) != 0);
    assert(state.filter_controller_21 == 0);
    assert((state.applyControlChange(21, 127) & ChannelControlFilterChanged) != 0);
    state.resetAll();
    assert(state.filter_controller_21 == 0);

    MidiChannelStateBank bank;
    assert((bank.applyControlChange(4, 21, 31) & ChannelControlFilterChanged) != 0);
    assert(bank.channel(4).filter_controller_21 == 31);
    assert(bank.channel(5).filter_controller_21 == 0);
    assert(bank.applyControlChange(16, 21, 20) == 0);
}

void testPitchBendSensitivityRpn() {
    MidiChannelState state{};
    state.resetAll();

    assert(state.applyControlChange(6, 12) == 0);
    assert(state.pitch_bend_range_semitones == 2);

    assert((state.applyControlChange(101, 0) & ChannelControlRpnSelectionChanged) != 0);
    assert((state.applyControlChange(100, 0) & ChannelControlRpnSelectionChanged) != 0);
    assert(state.pitchBendRpnSelected());

    assert((state.applyControlChange(6, 12) & ChannelControlPitchRangeChanged) != 0);
    assert((state.applyControlChange(38, 50) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 12);
    assert(state.pitch_bend_range_cents == 50);

    assert((state.applyControlChange(38, 127) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_cents == 99);

    assert((state.applyControlChange(96, 0) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 13);
    assert((state.applyControlChange(97, 0) & ChannelControlPitchRangeChanged) != 0);
    assert(state.pitch_bend_range_semitones == 12);

    state.applyControlChange(101, 127);
    state.applyControlChange(100, 127);
    assert(state.rpnNull());
    assert(state.applyControlChange(6, 3) == 0);
    assert(state.pitch_bend_range_semitones == 12);

    state.applyControlChange(99, 1);
    state.applyControlChange(98, 2);
    assert(state.applyControlChange(6, 7) == 0);
    assert(state.pitch_bend_range_semitones == 12);
}

void testResetAllControllersPreservesMixButResetsPerformanceState() {
    MidiChannelState state{};
    state.resetAll();
    state.applyControlChange(0, 3);
    state.applyControlChange(32, 4);
    state.applyProgramChange(22);
    state.applyControlChange(1, 88);
    assert(state.applyChannelPressure(77));
    state.applyControlChange(7, 77);
    state.applyControlChange(10, 31);
    state.applyControlChange(11, 45);
    state.applyControlChange(64, 127);
    state.applyControlChange(91, 55);
    state.applyControlChange(93, 66);
    state.applyControlChange(101, 0);
    state.applyControlChange(100, 0);
    state.applyControlChange(6, 12);
    state.applyControlChange(38, 50);
    assert(state.applyPitchBend(16383));

    const std::uint32_t action = state.applyControlChange(121, 0);
    assert((action & ChannelControlResetControllers) != 0);
    assert((action & ChannelControlGainChanged) != 0);
    assert((action & ChannelControlSustainChanged) != 0);
    assert((action & ChannelControlVibratoChanged) != 0);
    assert(state.selectedBank() == ((3u << 7u) | 4u));
    assert(state.program == 22);
    assert(state.volume == 77);
    assert(state.pan == 31);
    assert(state.reverb_send == 55);
    assert(state.chorus_send == 66);
    assert(state.modulation_wheel == 0);
    assert(state.channel_pressure == 0);
    assert(state.expression == 127);
    assert(!state.sustainDown());
    assert(state.pitch_bend == MidiChannelState::kPitchBendCenter);
    assert(state.pitch_bend_range_semitones == 12);
    assert(state.pitch_bend_range_cents == 50);
    assert(state.rpnNull());
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
    assert(bank.applyChannelPressure(2, 91));
    assert(bank.channel(2).channel_pressure == 91);
    assert(bank.channel(1).channel_pressure == 0);
    assert(bank.applyControlChange(16, 7, 10) == 0);
    assert(!bank.applyChannelPressure(16, 10));
    assert(!bank.applyPitchBend(16, 8192));
    assert(!bank.applyProgramChange(16, 1));
}

}  // namespace

int main() {
    testGmDefaults();
    testBankVolumePanExpressionSustainAndVibratoControllers();
    testCc21FilterControllerLifecycle();
    testPitchBendSensitivityRpn();
    testResetAllControllersPreservesMixButResetsPerformanceState();
    testPitchBendAndProgramValueValidation();
    testChannelBankIsIndependentAcross16Channels();
    std::cout << "GM MIDI channel-state host checks passed\n";
    return 0;
}
