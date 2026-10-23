#include "sf2/Sf2NoteOnModulator.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::evaluatePreparedNoteOnModulator;
using gm_synth::sf2::noteOnSourceToAmount;

namespace {

void testSingleSourceMatchesExistingEvaluator() {
    const Sf2PreparedNoteOnModulator route{0x0203u, 17u, 250, 0u};
    assert(route.valid());
    for (std::uint8_t key : {std::uint8_t{0}, std::uint8_t{60}, std::uint8_t{127}}) {
        for (std::uint8_t velocity : {std::uint8_t{1}, std::uint8_t{64}, std::uint8_t{127}}) {
            assert(evaluatePreparedNoteOnModulator(route, key, velocity) ==
                noteOnSourceToAmount(route.source_oper, key, velocity, route.amount));
        }
    }
}

void testFluidDualVelocityRoute() {
    // Observed in FluidR3: positive concave velocity -> initialFilterQ with a
    // negative switch velocity amount source. Low velocities retain the route;
    // high velocities gate it to zero.
    const Sf2PreparedNoteOnModulator route{0x0402u, 9u, -470, 0x0D02u};
    assert(route.valid());

    const std::int32_t low_primary = noteOnSourceToAmount(0x0402u, 60, 32, -470);
    const std::int32_t low_expected = noteOnSourceToAmount(0x0D02u, 60, 32, low_primary);
    assert(low_expected < 0);
    assert(evaluatePreparedNoteOnModulator(route, 60, 32) == low_expected);

    const std::int32_t high_primary = noteOnSourceToAmount(0x0402u, 60, 96, -470);
    const std::int32_t high_expected = noteOnSourceToAmount(0x0D02u, 60, 96, high_primary);
    assert(high_expected == 0);
    assert(evaluatePreparedNoteOnModulator(route, 60, 96) == 0);
}

void testIdentityAndSafetyBoundary() {
    const Sf2PreparedNoteOnModulator a{0x0002u, 11u, 4000, 0u};
    const Sf2PreparedNoteOnModulator same_identity{0x0002u, 11u, 5000, 0u};
    const Sf2PreparedNoteOnModulator different_amount_source{0x0002u, 11u, 4000, 0x0102u};
    const Sf2PreparedNoteOnModulator different_destination{0x0002u, 7u, 4000, 0u};
    const Sf2PreparedNoteOnModulator arbitrary_cc{0x0195u, 8u, -3800, 0u};

    assert(a.valid());
    assert(a.sameIdentity(same_identity));
    assert(!a.sameIdentity(different_amount_source));
    assert(!a.sameIdentity(different_destination));

    // Arbitrary CC state is intentionally outside the immutable NoteOn source
    // contract until MidiChannelState carries a general controller bank.
    assert(!arbitrary_cc.valid());
    assert(evaluatePreparedNoteOnModulator(arbitrary_cc, 60, 100) == 0);
}

}  // namespace

int main() {
    testSingleSourceMatchesExistingEvaluator();
    testFluidDualVelocityRoute();
    testIdentityAndSafetyBoundary();
    std::cout << "SF2 prepared NoteOn modulator checks passed\n";
    return 0;
}
