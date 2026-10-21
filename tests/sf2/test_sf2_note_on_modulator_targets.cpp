#include "sf2/Sf2NoteOnModulatorTargets.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using gm_synth::sf2::Sf2NoteOnTargetDeltas;
using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::applyPreparedNoteOnModulator;
using gm_synth::sf2::evaluatePreparedNoteOnModulator;
using gm_synth::sf2::isPreparedNoteOnDestination;

namespace {

void testObservedGeneralUserDestinationsAreRepresentable() {
    for (std::uint16_t destination : {
             std::uint16_t{0}, std::uint16_t{4}, std::uint16_t{7},
             std::uint16_t{9}, std::uint16_t{11}, std::uint16_t{17},
             std::uint16_t{28}, std::uint16_t{34}, std::uint16_t{36},
             std::uint16_t{38}, std::uint16_t{48}, std::uint16_t{52}}) {
        assert(isPreparedNoteOnDestination(destination));
    }
    assert(!isPreparedNoteOnDestination(8));  // existing dedicated filter-Fc path
}

void testFluidDualSourceContributionTargetsFilterQ() {
    const Sf2PreparedNoteOnModulator fluid{0x0402u, 9u, -470, 0x0D02u};
    Sf2NoteOnTargetDeltas low{};
    Sf2NoteOnTargetDeltas high{};
    assert(applyPreparedNoteOnModulator(fluid, 60, 32, &low));
    assert(applyPreparedNoteOnModulator(fluid, 60, 96, &high));
    assert(low.filter_q_cb == evaluatePreparedNoteOnModulator(fluid, 60, 32));
    assert(low.filter_q_cb < 0);
    assert(high.filter_q_cb == 0);
}

void testMultipleRoutesAccumulateInNativeDestinationUnits() {
    Sf2NoteOnTargetDeltas deltas{};
    const Sf2PreparedNoteOnModulator mod_env_pitch{0x0002u, 7u, 1200, 0u};
    const Sf2PreparedNoteOnModulator attenuation{0x0402u, 48u, 300, 0u};
    const Sf2PreparedNoteOnModulator fine_tune{0x0202u, 52u, 4, 0u};

    assert(applyPreparedNoteOnModulator(mod_env_pitch, 64, 100, &deltas));
    assert(applyPreparedNoteOnModulator(attenuation, 64, 100, &deltas));
    assert(applyPreparedNoteOnModulator(fine_tune, 64, 100, &deltas));
    assert(deltas.mod_env_pitch_cents ==
        evaluatePreparedNoteOnModulator(mod_env_pitch, 64, 100));
    assert(deltas.attenuation_cb ==
        evaluatePreparedNoteOnModulator(attenuation, 64, 100));
    assert(deltas.fine_tune_cents ==
        evaluatePreparedNoteOnModulator(fine_tune, 64, 100));
}

void testSampleAddressRoutesAccumulateInGeneratorUnits() {
    Sf2NoteOnTargetDeltas deltas{};
    const Sf2PreparedNoteOnModulator start_offset{0x0002u, 0u, -2000, 0u};
    const Sf2PreparedNoteOnModulator coarse_start{0x0102u, 4u, 1, 0u};
    assert(applyPreparedNoteOnModulator(start_offset, 60, 100, &deltas));
    assert(applyPreparedNoteOnModulator(coarse_start, 60, 100, &deltas));
    assert(deltas.sample_start_offset_points ==
        evaluatePreparedNoteOnModulator(start_offset, 60, 100));
    assert(deltas.sample_start_coarse_32k ==
        evaluatePreparedNoteOnModulator(coarse_start, 60, 100));
}

}  // namespace

int main() {
    testObservedGeneralUserDestinationsAreRepresentable();
    testFluidDualSourceContributionTargetsFilterQ();
    testMultipleRoutesAccumulateInNativeDestinationUnits();
    testSampleAddressRoutesAccumulateInGeneratorUnits();
    std::cout << "SF2 prepared NoteOn target checks passed\n";
    return 0;
}
