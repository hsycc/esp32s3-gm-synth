#pragma once

#include "Sf2FilterMath.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {

// SF2 initialFilterQ is stored in centibels. The filter's linear Q is
// 10^(Q_dB/20), so the denominator uses QInv = 10^(-centibels/200).
// This helper stays integer-only and is intended for NoteOn/control-domain use.
inline std::uint32_t resonanceQInvQ30(std::int32_t resonance_cb) {
    constexpr std::uint32_t kQ30One = std::uint32_t{1} << 30u;
    // round(10^(-1/200) * 2^30)
    constexpr std::uint32_t kOneCentibelQInvQ30 = 1061450803u;

    std::uint32_t exponent = static_cast<std::uint32_t>(
        std::clamp<std::int32_t>(resonance_cb, 0, 960));
    std::uint32_t result = kQ30One;
    std::uint32_t base = kOneCentibelQInvQ30;
    while (exponent != 0) {
        if ((exponent & 1u) != 0) {
            result = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(result) * base +
                 (std::uint64_t{1} << 29u)) >> 30u);
        }
        exponent >>= 1u;
        if (exponent != 0) {
            base = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(base) * base +
                 (std::uint64_t{1} << 29u)) >> 30u);
        }
    }
    return result;
}

// The SoundFont spec also reduces DC gain by half of the resonance amount.
// The runtime attenuation summing node is integer centibels, so odd values are
// rounded to the nearest representable 0.1 dB step (maximum error 0.05 dB).
inline std::int32_t resonanceDcAttenuationCb(std::int32_t resonance_cb) {
    const std::int32_t clamped = std::clamp<std::int32_t>(resonance_cb, 0, 960);
    return (clamped + 1) / 2;
}

inline std::int32_t divideQ24ByPositiveQ24(
    std::int32_t numerator_q24,
    std::uint32_t denominator_q24) {
    if (denominator_q24 == 0) {
        return 0;
    }
    constexpr std::uint64_t kOneQ24 = std::uint64_t{1} << 24u;
    const bool negative = numerator_q24 < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(numerator_q24))
        : static_cast<std::uint64_t>(numerator_q24);
    const std::uint64_t scaled = magnitude * kOneQ24;
    const std::uint64_t rounded = (scaled + denominator_q24 / 2u) / denominator_q24;
    const std::int64_t signed_value = negative
        ? -static_cast<std::int64_t>(rounded)
        : static_cast<std::int64_t>(rounded);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        signed_value,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

// Reuses the already-qualified Q=0 cutoff table, then changes only the terms
// affected by Q. For the K=tan(pi*Fc/Fs) topology used by Sf2FilterMath:
//   D0 = 1 + K + K^2
//   r  = K / D0 = (1 - b2_0) / 2
//   Dq / D0 = 1 - r + QInv*r
// This avoids tan()/pow() in the realtime path and preserves the existing Q24
// coefficient/storage format for the complete SF2 0..960 cB range.
inline bool prepareResonantLowpassQ24(
    std::int32_t cutoff_cents,
    std::int32_t resonance_cb,
    std::uint32_t output_sample_rate,
    voice::BiquadLowpassConfig* output) {
    if (output == nullptr || resonance_cb < 0 || resonance_cb > 960) {
        return false;
    }

    voice::BiquadLowpassConfig base{};
    if (!prepareLowpassQ24(cutoff_cents, 0, output_sample_rate, &base)) {
        return false;
    }
    if (resonance_cb == 0) {
        *output = base;
        return true;
    }

    constexpr std::int64_t kOneQ24 = std::int64_t{1} << 24u;
    const std::int64_t r_q24 =
        (kOneQ24 - static_cast<std::int64_t>(base.b2_q24) + 1) / 2;
    if (r_q24 <= 0 || r_q24 >= kOneQ24) {
        return false;
    }

    const std::uint32_t qinv_q30 = resonanceQInvQ30(resonance_cb);
    const std::int64_t r_qinv_q24 = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(r_q24) * qinv_q30 +
         (std::uint64_t{1} << 29u)) >> 30u);
    const std::int64_t scale_q24 = kOneQ24 - r_q24 + r_qinv_q24;
    if (scale_q24 <= 0 || scale_q24 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const std::int64_t b2_numerator_q24 = kOneQ24 - r_q24 - r_qinv_q24;
    if (b2_numerator_q24 < std::numeric_limits<std::int32_t>::min() ||
        b2_numerator_q24 > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    voice::BiquadLowpassConfig candidate{};
    candidate.enabled = true;
    candidate.a0_q24 = divideQ24ByPositiveQ24(
        base.a0_q24,
        static_cast<std::uint32_t>(scale_q24));
    candidate.a1_q24 = divideQ24ByPositiveQ24(
        base.a1_q24,
        static_cast<std::uint32_t>(scale_q24));
    candidate.b1_q24 = divideQ24ByPositiveQ24(
        base.b1_q24,
        static_cast<std::uint32_t>(scale_q24));
    candidate.b2_q24 = divideQ24ByPositiveQ24(
        static_cast<std::int32_t>(b2_numerator_q24),
        static_cast<std::uint32_t>(scale_q24));
    if (!candidate.valid()) {
        return false;
    }

    *output = candidate;
    return true;
}

}  // namespace gm_synth::sf2
