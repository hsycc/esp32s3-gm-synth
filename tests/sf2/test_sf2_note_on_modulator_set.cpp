#include "sf2/Sf2NoteOnModulatorSet.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::Sf2PreparedNoteOnModulatorSet;
using gm_synth::sf2::addPreparedNoteOnModulator;
using gm_synth::sf2::combinePreparedNoteOnLevels;
using gm_synth::sf2::replacePreparedNoteOnModulator;

namespace {

Sf2PreparedNoteOnModulator route(
    std::uint16_t source,
    std::uint16_t destination,
    std::int32_t amount,
    std::uint16_t amount_source = 0) {
    return Sf2PreparedNoteOnModulator{source, destination, amount, amount_source};
}

void testLocalReplacesGlobalIdentity() {
    Sf2PreparedNoteOnModulatorSet preset_global{};
    Sf2PreparedNoteOnModulatorSet preset_local{};
    Sf2PreparedNoteOnModulatorSet instrument_global{};
    Sf2PreparedNoteOnModulatorSet instrument_local{};
    Sf2PreparedNoteOnModulatorSet combined{};

    assert(replacePreparedNoteOnModulator(
        &instrument_global, route(0x0402u, 9u, -470, 0x0D02u)));
    assert(replacePreparedNoteOnModulator(
        &instrument_local, route(0x0402u, 9u, -300, 0x0D02u)));
    assert(combinePreparedNoteOnLevels(
        preset_global, preset_local, instrument_global, instrument_local, &combined));
    assert(combined.count == 1);
    assert(combined.routes[0].amount == -300);
}

void testPresetAddsAfterItsOwnReplacement() {
    Sf2PreparedNoteOnModulatorSet preset_global{};
    Sf2PreparedNoteOnModulatorSet preset_local{};
    Sf2PreparedNoteOnModulatorSet instrument_global{};
    Sf2PreparedNoteOnModulatorSet instrument_local{};
    Sf2PreparedNoteOnModulatorSet combined{};

    assert(replacePreparedNoteOnModulator(
        &instrument_local, route(0x0002u, 11u, 3000)));
    assert(replacePreparedNoteOnModulator(
        &preset_global, route(0x0002u, 11u, 1000)));
    assert(replacePreparedNoteOnModulator(
        &preset_local, route(0x0002u, 11u, 2000)));
    assert(combinePreparedNoteOnLevels(
        preset_global, preset_local, instrument_global, instrument_local, &combined));
    assert(combined.count == 1);
    assert(combined.routes[0].amount == 5000);
}

void testAmountSourceParticipatesInIdentity() {
    Sf2PreparedNoteOnModulatorSet set{};
    assert(addPreparedNoteOnModulator(&set, route(0x0002u, 9u, 100)));
    assert(addPreparedNoteOnModulator(&set, route(0x0002u, 9u, 200, 0x0102u)));
    assert(set.count == 2);
}

void testSaturatingPresetAddition() {
    Sf2PreparedNoteOnModulatorSet preset_local{};
    Sf2PreparedNoteOnModulatorSet instrument_local{};
    Sf2PreparedNoteOnModulatorSet empty{};
    Sf2PreparedNoteOnModulatorSet combined{};

    assert(replacePreparedNoteOnModulator(
        &instrument_local,
        route(0x0002u, 48u, std::numeric_limits<std::int32_t>::max() - 10)));
    assert(replacePreparedNoteOnModulator(
        &preset_local, route(0x0002u, 48u, 100)));
    assert(combinePreparedNoteOnLevels(
        empty, preset_local, empty, instrument_local, &combined));
    assert(combined.count == 1);
    assert(combined.routes[0].amount == std::numeric_limits<std::int32_t>::max());
}

}  // namespace

int main() {
    testLocalReplacesGlobalIdentity();
    testPresetAddsAfterItsOwnReplacement();
    testAmountSourceParticipatesInIdentity();
    testSaturatingPresetAddition();
    std::cout << "SF2 prepared NoteOn modulator hierarchy checks passed\n";
    return 0;
}
