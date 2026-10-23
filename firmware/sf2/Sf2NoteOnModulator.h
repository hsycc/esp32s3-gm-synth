#pragma once

#include "Sf2ModulationMath.h"

#include <cstdint>

namespace gm_synth::sf2 {

// Control-domain representation of one SoundFont modulator whose source and
// optional amount-source are immutable NoteOn inputs (key or velocity). The
// scanner must reject non-zero transform operators before publishing this form.
// Destination interpretation stays outside this type so generator-specific
// clamps remain owned by the existing playback/voice code.
struct Sf2PreparedNoteOnModulator {
    std::uint16_t source_oper = 0;
    std::uint16_t destination_oper = 0;
    std::int32_t amount = 0;
    std::uint16_t amount_source_oper = 0;

    bool valid() const {
        return isSupportedSf2NoteOnSourceOper(source_oper) &&
            (amount_source_oper == 0 ||
             isSupportedSf2NoteOnSourceOper(amount_source_oper));
    }

    bool sameIdentity(const Sf2PreparedNoteOnModulator& other) const {
        return source_oper == other.source_oper &&
            destination_oper == other.destination_oper &&
            amount_source_oper == other.amount_source_oper;
    }
};

// SoundFont applies the primary source and then multiplies it by the optional
// amount source. noteOnSourceToAmount already evaluates a signed normalized
// source against an arbitrary signed amount, so applying it a second time gives
// the same bounded fixed-point composition without introducing floating point
// into the realtime NoteOn path.
inline std::int32_t evaluatePreparedNoteOnModulator(
    const Sf2PreparedNoteOnModulator& route,
    std::uint8_t key,
    std::uint8_t velocity) {
    if (!route.valid()) {
        return 0;
    }
    std::int32_t value = noteOnSourceToAmount(
        route.source_oper,
        key,
        velocity,
        route.amount);
    if (route.amount_source_oper != 0) {
        value = noteOnSourceToAmount(
            route.amount_source_oper,
            key,
            velocity,
            value);
    }
    return value;
}

}  // namespace gm_synth::sf2
