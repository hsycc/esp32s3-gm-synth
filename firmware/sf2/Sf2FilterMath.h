#pragma once

#include "Sf2NoteOnModulatorSet.h"
#include "voice/VoiceRenderer.h"

#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {

constexpr std::int32_t kSf2DefaultVelocityFilterCutoffCents = -2400;
constexpr std::int32_t kSf2MinFilterCutoffCents = 1500;
constexpr std::int32_t kSf2MaxFilterCutoffCents = 13500;
constexpr std::uint32_t kSf2FilterOutputSampleRate = 44100;
constexpr std::uint16_t kSf2InitialFilterQDestination = 9u;

// SoundFont 2.04 default velocity->initialFilterFc route: negative-unipolar
// linear Note-On velocity, no secondary source, amount -2400 cents. The
// endpoints follow the SF2 controller domain (1 -> 127/128, 127 -> 0).
std::int32_t velocityToFilterCutoffCents(
    std::uint8_t velocity,
    std::int32_t amount_cents = kSf2DefaultVelocityFilterCutoffCents);

inline bool isVelocityOnlySf2NoteOnSourceOper(std::uint16_t source_oper) {
    Sf2SourceDescriptor source{};
    return decodeSf2SourceOper(source_oper, &source) &&
        !source.midi_cc && source.index == 2u;
}

// Prepared FilterQ NoteOn modulation keeps the historical single negative-
// linear velocity amount plus the small set of real-bank dual-source velocity
// routes. The type stays assignment-compatible with the old int32 field so the
// materializer and existing direct tests retain their old surface.
struct Sf2FilterQNoteOnModulation {
    std::int32_t velocity_amount_cb = 0;
    Sf2PreparedNoteOnModulatorSet additional_routes{};

    Sf2FilterQNoteOnModulation() = default;
    Sf2FilterQNoteOnModulation(std::int32_t amount_cb)
        : velocity_amount_cb(amount_cb) {}

    Sf2FilterQNoteOnModulation& operator=(std::int32_t amount_cb) {
        velocity_amount_cb = amount_cb;
        additional_routes = Sf2PreparedNoteOnModulatorSet{};
        return *this;
    }

    operator std::int32_t() const { return velocity_amount_cb; }

    static bool representedFilterQRoute(const Sf2PreparedNoteOnModulator& route) {
        return route.destination_oper == kSf2InitialFilterQDestination &&
            isVelocityOnlySf2NoteOnSourceOper(route.source_oper) &&
            (route.amount_source_oper == 0 ||
             isVelocityOnlySf2NoteOnSourceOper(route.amount_source_oper));
    }

    bool fullyRepresentedFilterQOnly() const {
        if (!additional_routes.valid()) {
            return false;
        }
        for (std::size_t index = 0; index < additional_routes.count; ++index) {
            if (!representedFilterQRoute(additional_routes.routes[index])) {
                return false;
            }
        }
        return true;
    }

    // Structural validity stays separate from qualification. Preview-mode
    // programs may retain prepared routes that are still unsupported; those
    // routes are ignored by the FilterQ evaluator but remain explicit through
    // ResolvedZoneModulation::hasUnsupportedModulators().
    bool valid() const { return additional_routes.valid(); }
};

// FilterQ uses the same historical negative-linear velocity helper for its
// legacy scalar route. Real FluidR3 programs additionally use a dual-source
// NoteOn velocity route. Evaluating the prepared routes here lets the existing
// ZoneResolver call site remain unchanged and keeps all source-curve math in the
// SF2 math layer.
inline std::int32_t velocityToFilterCutoffCents(
    std::uint8_t velocity,
    const Sf2FilterQNoteOnModulation& modulation) {
    std::int64_t summed = velocityToFilterCutoffCents(
        velocity,
        modulation.velocity_amount_cb);
    for (std::size_t index = 0; index < modulation.additional_routes.count; ++index) {
        const Sf2PreparedNoteOnModulator& route = modulation.additional_routes.routes[index];
        if (!Sf2FilterQNoteOnModulation::representedFilterQRoute(route)) {
            continue;
        }
        summed += evaluatePreparedNoteOnModulator(route, 0, velocity);
    }
    if (summed > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (summed < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(summed);
}

// Builds the 12 dB/octave low-pass used by the current embedded profile. The
// coefficient path is integer/table based and is safe to execute at NoteOn.
// For now only 44.1 kHz and zero-cB resonance are qualified; callers must keep
// non-zero initialFilterQ explicit as pending rather than silently approximating
// it. Coefficients follow the transposed-direct-form-II topology used by
// TinySoundFont's reference low-pass implementation.
bool prepareLowpassQ24(
    std::int32_t cutoff_cents,
    std::int32_t resonance_cb,
    std::uint32_t output_sample_rate,
    voice::BiquadLowpassConfig* output);

}  // namespace gm_synth::sf2
