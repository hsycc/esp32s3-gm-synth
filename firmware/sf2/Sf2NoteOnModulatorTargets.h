#pragma once

#include "Sf2NoteOnModulator.h"

#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {

// Generator-domain deltas produced by immutable NoteOn key/velocity modulators.
// Units deliberately stay native to each SF2 destination so the existing
// destination-specific clamp/conversion remains authoritative when this state
// is integrated into PlaybackZone/ZoneResolver.
struct Sf2NoteOnTargetDeltas {
    std::int32_t sample_start_offset_points = 0;   // generator 0
    std::int32_t sample_start_coarse_32k = 0;      // generator 4
    std::int32_t mod_env_pitch_cents = 0;          // generator 7
    std::int32_t filter_q_cb = 0;                  // generator 9
    std::int32_t mod_env_filter_cents = 0;         // generator 11
    std::int32_t pan_sf2_units = 0;                // generator 17
    std::int32_t mod_env_decay_timecents = 0;      // generator 28
    std::int32_t vol_env_attack_timecents = 0;     // generator 34
    std::int32_t vol_env_decay_timecents = 0;      // generator 36
    std::int32_t vol_env_release_timecents = 0;    // generator 38
    std::int32_t attenuation_cb = 0;               // generator 48
    std::int32_t fine_tune_cents = 0;              // generator 52
};

inline std::int32_t saturatingNoteOnTargetAdd(std::int32_t a, std::int32_t b) {
    const std::int64_t sum = static_cast<std::int64_t>(a) + b;
    if (sum > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (sum < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(sum);
}

inline bool isPreparedNoteOnDestination(std::uint16_t destination_oper) {
    switch (destination_oper) {
        case 0:
        case 4:
        case 7:
        case 9:
        case 11:
        case 17:
        case 28:
        case 34:
        case 36:
        case 38:
        case 48:
        case 52:
            return true;
        default:
            return false;
    }
}

// Destinations whose generator-domain value is already represented by
// PlaybackZone and can therefore be evaluated once at NoteOn. Sample-start
// address offsets (0/4) are applied to the prepared SampleRegion with a final
// realizable-region clamp. FilterQ (9) keeps its dedicated payload so the
// already-qualified FluidR3 path is not double-counted. Pan (17) remains
// pending until stereo-side base pan and the generator clamp are represented
// in the correct SF2 order.
inline bool isRuntimeGenericPreparedNoteOnDestination(std::uint16_t destination_oper) {
    switch (destination_oper) {
        case 0:
        case 4:
        case 7:
        case 11:
        case 28:
        case 34:
        case 36:
        case 38:
        case 48:
        case 52:
            return true;
        default:
            return false;
    }
}

inline bool isRuntimeGenericPreparedNoteOnRoute(
    const Sf2PreparedNoteOnModulator& route) {
    return route.valid() &&
        isRuntimeGenericPreparedNoteOnDestination(route.destination_oper);
}

inline bool applyPreparedNoteOnModulator(
    const Sf2PreparedNoteOnModulator& route,
    std::uint8_t key,
    std::uint8_t velocity,
    Sf2NoteOnTargetDeltas* targets) {
    if (targets == nullptr || !route.valid() ||
        !isPreparedNoteOnDestination(route.destination_oper)) {
        return false;
    }

    const std::int32_t amount = evaluatePreparedNoteOnModulator(
        route,
        key,
        velocity);
    std::int32_t* destination = nullptr;
    switch (route.destination_oper) {
        case 0: destination = &targets->sample_start_offset_points; break;
        case 4: destination = &targets->sample_start_coarse_32k; break;
        case 7: destination = &targets->mod_env_pitch_cents; break;
        case 9: destination = &targets->filter_q_cb; break;
        case 11: destination = &targets->mod_env_filter_cents; break;
        case 17: destination = &targets->pan_sf2_units; break;
        case 28: destination = &targets->mod_env_decay_timecents; break;
        case 34: destination = &targets->vol_env_attack_timecents; break;
        case 36: destination = &targets->vol_env_decay_timecents; break;
        case 38: destination = &targets->vol_env_release_timecents; break;
        case 48: destination = &targets->attenuation_cb; break;
        case 52: destination = &targets->fine_tune_cents; break;
        default: return false;
    }
    *destination = saturatingNoteOnTargetAdd(*destination, amount);
    return true;
}

}  // namespace gm_synth::sf2
