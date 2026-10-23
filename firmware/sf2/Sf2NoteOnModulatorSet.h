#pragma once

#include "Sf2NoteOnModulator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {

constexpr std::size_t kSf2MaxPreparedNoteOnModulatorsPerDestination = 4;

struct Sf2PreparedNoteOnModulatorSet {
    std::array<
        Sf2PreparedNoteOnModulator,
        kSf2MaxPreparedNoteOnModulatorsPerDestination> routes{};
    std::uint8_t count = 0;

    bool valid() const {
        if (count > routes.size()) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!routes[index].valid()) {
                return false;
            }
        }
        return true;
    }
};

inline std::int32_t sf2SaturatingAddI32(std::int32_t a, std::int32_t b) {
    const std::int64_t sum = static_cast<std::int64_t>(a) + b;
    if (sum > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (sum < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(sum);
}

inline bool replacePreparedNoteOnModulator(
    Sf2PreparedNoteOnModulatorSet* set,
    const Sf2PreparedNoteOnModulator& route) {
    if (set == nullptr || !route.valid()) {
        return false;
    }
    for (std::size_t index = 0; index < set->count; ++index) {
        if (set->routes[index].sameIdentity(route)) {
            set->routes[index] = route;
            return true;
        }
    }
    if (set->count >= set->routes.size()) {
        return false;
    }
    set->routes[set->count++] = route;
    return true;
}

inline bool addPreparedNoteOnModulator(
    Sf2PreparedNoteOnModulatorSet* set,
    const Sf2PreparedNoteOnModulator& route) {
    if (set == nullptr || !route.valid()) {
        return false;
    }
    for (std::size_t index = 0; index < set->count; ++index) {
        if (set->routes[index].sameIdentity(route)) {
            set->routes[index].amount = sf2SaturatingAddI32(
                set->routes[index].amount,
                route.amount);
            return true;
        }
    }
    if (set->count >= set->routes.size()) {
        return false;
    }
    set->routes[set->count++] = route;
    return true;
}

inline bool mergePreparedNoteOnLevel(
    const Sf2PreparedNoteOnModulatorSet& global,
    const Sf2PreparedNoteOnModulatorSet& local,
    Sf2PreparedNoteOnModulatorSet* output) {
    if (output == nullptr || !global.valid() || !local.valid()) {
        return false;
    }
    *output = Sf2PreparedNoteOnModulatorSet{};
    for (std::size_t index = 0; index < global.count; ++index) {
        if (!replacePreparedNoteOnModulator(output, global.routes[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < local.count; ++index) {
        if (!replacePreparedNoteOnModulator(output, local.routes[index])) {
            return false;
        }
    }
    return true;
}

// SoundFont hierarchy semantics used by the existing resolver:
// instrument local replaces matching instrument global identities; preset local
// replaces matching preset global identities; the surviving preset modulator is
// then additive to the surviving instrument/default route.
inline bool combinePreparedNoteOnLevels(
    const Sf2PreparedNoteOnModulatorSet& preset_global,
    const Sf2PreparedNoteOnModulatorSet& preset_local,
    const Sf2PreparedNoteOnModulatorSet& instrument_global,
    const Sf2PreparedNoteOnModulatorSet& instrument_local,
    Sf2PreparedNoteOnModulatorSet* output) {
    if (output == nullptr) {
        return false;
    }
    Sf2PreparedNoteOnModulatorSet preset{};
    Sf2PreparedNoteOnModulatorSet instrument{};
    if (!mergePreparedNoteOnLevel(preset_global, preset_local, &preset) ||
        !mergePreparedNoteOnLevel(instrument_global, instrument_local, &instrument)) {
        return false;
    }

    *output = Sf2PreparedNoteOnModulatorSet{};
    for (std::size_t index = 0; index < instrument.count; ++index) {
        if (!replacePreparedNoteOnModulator(output, instrument.routes[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < preset.count; ++index) {
        if (!addPreparedNoteOnModulator(output, preset.routes[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace gm_synth::sf2
