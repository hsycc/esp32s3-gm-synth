#include "sf2/Sf2FilterMath.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using gm_synth::sf2::Sf2FilterQNoteOnModulation;
using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::addPreparedNoteOnModulator;
using gm_synth::sf2::evaluatePreparedNoteOnModulator;
using gm_synth::sf2::velocityToFilterCutoffCents;

namespace {

void testLegacyScalarCompatibility() {
    Sf2FilterQNoteOnModulation modulation{};
    modulation = -240;
    assert(modulation.valid());
    assert(static_cast<std::int32_t>(modulation) == -240);
    assert(velocityToFilterCutoffCents(32, modulation) ==
           velocityToFilterCutoffCents(32, -240));

    modulation.additional_routes.count = 1;
    modulation = 120;
    assert(modulation.additional_routes.count == 0);
    assert(static_cast<std::int32_t>(modulation) == 120);
}

void testFluidDualVelocityFilterQRoute() {
    Sf2FilterQNoteOnModulation modulation{};
    const Sf2PreparedNoteOnModulator route{
        0x0402u,  // positive-unipolar concave NoteOn velocity
        9u,       // initialFilterQ
        -470,
        0x0d02u,  // negative-unipolar switch NoteOn velocity amount source
    };
    assert(route.valid());
    assert(addPreparedNoteOnModulator(&modulation.additional_routes, route));
    assert(modulation.valid());
    assert(modulation.fullyRepresentedFilterQOnly());

    for (std::uint8_t velocity : {std::uint8_t{1}, std::uint8_t{32},
                                  std::uint8_t{63}, std::uint8_t{64},
                                  std::uint8_t{96}, std::uint8_t{127}}) {
        const std::int32_t expected = evaluatePreparedNoteOnModulator(
            route,
            0,
            velocity);
        assert(velocityToFilterCutoffCents(velocity, modulation) == expected);
    }
}

void testLegacyAndPreparedContributionsAdd() {
    Sf2FilterQNoteOnModulation modulation{};
    modulation.velocity_amount_cb = 80;
    const Sf2PreparedNoteOnModulator route{0x0402u, 9u, -470, 0x0d02u};
    assert(addPreparedNoteOnModulator(&modulation.additional_routes, route));

    for (std::uint8_t velocity : {std::uint8_t{12}, std::uint8_t{48},
                                  std::uint8_t{80}, std::uint8_t{120}}) {
        const std::int32_t expected =
            velocityToFilterCutoffCents(velocity, modulation.velocity_amount_cb) +
            evaluatePreparedNoteOnModulator(route, 0, velocity);
        assert(velocityToFilterCutoffCents(velocity, modulation) == expected);
    }
}

void testFilterQPayloadRejectsWrongDestinationOrKeySource() {
    Sf2FilterQNoteOnModulation wrong_destination{};
    assert(addPreparedNoteOnModulator(
        &wrong_destination.additional_routes,
        Sf2PreparedNoteOnModulator{0x0402u, 11u, -470, 0x0d02u}));
    assert(wrong_destination.valid());
    assert(!wrong_destination.fullyRepresentedFilterQOnly());

    Sf2FilterQNoteOnModulation key_source{};
    assert(addPreparedNoteOnModulator(
        &key_source.additional_routes,
        Sf2PreparedNoteOnModulator{0x0003u, 9u, 120, 0u}));
    assert(key_source.valid());
    assert(!key_source.fullyRepresentedFilterQOnly());
}

}  // namespace

int main() {
    testLegacyScalarCompatibility();
    testFluidDualVelocityFilterQRoute();
    testLegacyAndPreparedContributionsAdd();
    testFilterQPayloadRejectsWrongDestinationOrKeySource();
    std::cout << "SF2 FilterQ prepared NoteOn modulation checks passed\n";
    return 0;
}
