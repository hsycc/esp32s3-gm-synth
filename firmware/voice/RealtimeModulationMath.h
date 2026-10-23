#pragma once

#include "VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace gm_synth::voice::realtime_modulation {
namespace detail {

constexpr std::array<std::uint32_t, 12> kSemitoneRatioQ20{
    1048576u, 1110928u, 1176987u, 1246974u,
    1321123u, 1399681u, 1482910u, 1571089u,
    1664511u, 1763488u, 1868350u, 1979448u,
};
constexpr std::array<std::uint32_t, 100> kCentRatioQ20{
    1048576u, 1049182u, 1049788u, 1050395u, 1051002u, 1051609u, 1052216u, 1052824u, 1053433u, 1054041u,
    1054650u, 1055260u, 1055869u, 1056479u, 1057090u, 1057701u, 1058312u, 1058923u, 1059535u, 1060147u,
    1060760u, 1061373u, 1061986u, 1062600u, 1063214u, 1063828u, 1064443u, 1065058u, 1065673u, 1066289u,
    1066905u, 1067521u, 1068138u, 1068755u, 1069373u, 1069991u, 1070609u, 1071227u, 1071846u, 1072466u,
    1073085u, 1073705u, 1074326u, 1074946u, 1075568u, 1076189u, 1076811u, 1077433u, 1078055u, 1078678u,
    1079302u, 1079925u, 1080549u, 1081174u, 1081798u, 1082423u, 1083049u, 1083674u, 1084301u, 1084927u,
    1085554u, 1086181u, 1086809u, 1087437u, 1088065u, 1088694u, 1089323u, 1089952u, 1090582u, 1091212u,
    1091843u, 1092473u, 1093105u, 1093736u, 1094368u, 1095000u, 1095633u, 1096266u, 1096900u, 1097533u,
    1098167u, 1098802u, 1099437u, 1100072u, 1100708u, 1101344u, 1101980u, 1102617u, 1103254u, 1103891u,
    1104529u, 1105167u, 1105806u, 1106445u, 1107084u, 1107724u, 1108364u, 1109004u, 1109645u, 1110286u,
};

constexpr std::uint32_t kQ15Divisor = 32767u;
constexpr std::uint32_t kQ15Round = kQ15Divisor / 2u;
constexpr std::uint32_t kRatioFractionBits = 20u;
constexpr std::uint64_t kRatioRounding = std::uint64_t{1} << (kRatioFractionBits - 1u);
constexpr std::uint32_t kTableSegments =
    static_cast<std::uint32_t>(kLfoControlPointCount - 1u);
constexpr std::uint32_t kTableScaleQ16 = kTableSegments << 16u;

inline std::uint32_t magnitudeI32(std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    return value < 0 ? 0u - bits : bits;
}

// Exact replacement for round(value * q15 / 32767), but the only divides are
// 32-bit. Splitting the magnitude by 32767 before multiplying preserves the
// original half-away-from-zero result while avoiding Xtensa __divdi3.
inline std::int32_t scaleSignedQ15(std::int32_t value, std::int32_t q15) {
    const bool negative = (value < 0) != (q15 < 0);
    const std::uint32_t magnitude = magnitudeI32(value);
    const std::uint32_t q15_magnitude = magnitudeI32(q15);
    const std::uint32_t whole = magnitude / kQ15Divisor;
    const std::uint32_t remainder = magnitude - whole * kQ15Divisor;
    const std::uint64_t scaled_magnitude =
        static_cast<std::uint64_t>(whole) * q15_magnitude +
        (remainder * q15_magnitude + kQ15Round) / kQ15Divisor;
    const std::int64_t signed_value = negative
        ? -static_cast<std::int64_t>(scaled_magnitude)
        : static_cast<std::int64_t>(scaled_magnitude);
    return static_cast<std::int32_t>(signed_value);
}

inline std::int32_t triangleLevelQ15(std::uint32_t phase_q32) {
    constexpr std::uint32_t kQuarterMask = (std::uint32_t{1} << 30u) - 1u;
    const std::uint32_t quadrant = phase_q32 >> 30u;
    const std::uint32_t fraction = phase_q32 & kQuarterMask;
    const std::int32_t ramp = static_cast<std::int32_t>(
        (static_cast<std::uint64_t>(fraction) * kQ15Unity +
         (std::uint64_t{1} << 29u)) >> 30u);
    switch (quadrant) {
        case 0:
            return ramp;
        case 1:
            return kQ15Unity - ramp;
        case 2:
            return -ramp;
        default:
            return -kQ15Unity + ramp;
    }
}

struct LfoTablePosition {
    std::size_t lower = 0;
    std::uint32_t fraction_q16 = 0;
};

inline LfoTablePosition lfoTablePosition(std::int32_t level_q15) {
    const std::int32_t clamped = std::clamp(level_q15, -kQ15Unity, kQ15Unity);
    const std::uint32_t normalized = static_cast<std::uint32_t>(clamped + kQ15Unity);
    constexpr std::uint32_t kSpan = static_cast<std::uint32_t>(kQ15Unity) * 2u;

    // round(normalized * 8 * 65536 / 65534), decomposed so every product and
    // divide is 32-bit and the result stays bit-identical to the reference.
    const std::uint32_t scaled_segments = normalized * kTableSegments;
    const std::uint32_t whole = scaled_segments / kSpan;
    const std::uint32_t remainder = scaled_segments - whole * kSpan;
    const std::uint32_t fraction =
        (remainder * 65536u + kSpan / 2u) / kSpan;
    const std::uint32_t position_q16 = whole * 65536u + fraction;
    if (position_q16 >= kTableScaleQ16) {
        return LfoTablePosition{kLfoControlPointCount - 1u, 0};
    }
    return LfoTablePosition{
        static_cast<std::size_t>(position_q16 >> 16u),
        position_q16 & 0xffffu,
    };
}

inline std::int32_t interpolateI32(
    std::int32_t a,
    std::int32_t b,
    std::uint32_t fraction_q16) {
    const std::int64_t delta = static_cast<std::int64_t>(b) - a;
    const std::int64_t scaled = delta * fraction_q16;
    const std::int64_t rounded = scaled >= 0
        ? (scaled + 32768) >> 16u
        : -(((-scaled) + 32768) >> 16u);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(a) + rounded,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

inline StereoGainQ15 interpolateGainTable(
    const LfoGainTable& table,
    std::int32_t level_q15) {
    if (!table.enabled) {
        return StereoGainQ15{};
    }
    const LfoTablePosition position = lfoTablePosition(level_q15);
    if (position.lower + 1u >= table.points.size()) {
        return table.points.back();
    }
    const StereoGainQ15& a = table.points[position.lower];
    const StereoGainQ15& b = table.points[position.lower + 1u];
    return StereoGainQ15{
        interpolateI32(a.left, b.left, position.fraction_q16),
        interpolateI32(a.right, b.right, position.fraction_q16),
    };
}

// Exact rounded Q19 ratio for 0 <= numerator <= denominator. The common SF2
// dynamic-filter span is small enough for one 32-bit divide. Very wide legal
// int32 spans use binary long division, keeping the fallback exact without any
// 64-bit divide helper.
inline std::uint32_t ratioQ19(std::uint32_t numerator, std::uint32_t denominator) {
    if (denominator == 0u || numerator == 0u) {
        return 0u;
    }
    if (numerator >= denominator) {
        return std::uint32_t{1} << 19u;
    }
    if (denominator <= 8191u) {
        return (numerator * (std::uint32_t{1} << 19u) + denominator / 2u) /
            denominator;
    }

    std::uint32_t quotient = 0u;
    std::uint32_t remainder = numerator;
    for (unsigned bit = 0; bit < 19u; ++bit) {
        quotient <<= 1u;
        const std::uint32_t gap = denominator - remainder;
        if (remainder >= gap) {
            remainder -= gap;
            quotient |= 1u;
        } else {
            remainder += remainder;
        }
    }
    if (remainder >= denominator - remainder) {
        ++quotient;
    }
    return quotient;
}

inline BiquadLowpassConfig interpolateDynamicLowpassTable(
    const DynamicLowpassTable& table,
    std::int32_t offset_cents) {
    if (!table.enabled) {
        return BiquadLowpassConfig{};
    }
    if (table.min_offset_cents == table.max_offset_cents) {
        return table.points.front();
    }

    const std::int32_t clamped = std::clamp(
        offset_cents,
        table.min_offset_cents,
        table.max_offset_cents);
    const std::uint32_t normalized = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(clamped) - table.min_offset_cents);
    const std::uint32_t span = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(table.max_offset_cents) - table.min_offset_cents);
    const std::uint32_t position_q16 = ratioQ19(normalized, span);
    const std::size_t lower = std::min<std::size_t>(
        static_cast<std::size_t>(position_q16 >> 16u),
        table.points.size() - 1u);
    if (lower + 1u >= table.points.size()) {
        return table.points.back();
    }
    const std::uint32_t fraction_q16 = position_q16 & 0xffffu;
    const BiquadLowpassConfig& a = table.points[lower];
    const BiquadLowpassConfig& b = table.points[lower + 1u];
    BiquadLowpassConfig result{};
    result.enabled = true;
    result.a0_q24 = interpolateI32(a.a0_q24, b.a0_q24, fraction_q16);
    result.a1_q24 = interpolateI32(a.a1_q24, b.a1_q24, fraction_q16);
    result.b1_q24 = interpolateI32(a.b1_q24, b.b1_q24, fraction_q16);
    result.b2_q24 = interpolateI32(a.b2_q24, b.b2_q24, fraction_q16);
    return result;
}

inline std::uint64_t multiplyRatioQ20(
    std::uint64_t value,
    std::uint32_t ratio_q20) {
    if (value == 0u || ratio_q20 == 0u) {
        return 0u;
    }

    // Form the full 64-bit product from two 32x32 products. Detecting overflow
    // from the upper half is equivalent to the old max/ratio guard, but avoids
    // the variable __udivdi3 call on Xtensa.
    const std::uint32_t value_low = static_cast<std::uint32_t>(value);
    const std::uint32_t value_high = static_cast<std::uint32_t>(value >> 32u);
    const std::uint64_t low_product =
        static_cast<std::uint64_t>(value_low) * ratio_q20;
    const std::uint64_t high_product =
        static_cast<std::uint64_t>(value_high) * ratio_q20;
    const std::uint64_t high_total = high_product + (low_product >> 32u);
    if (high_total > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    const std::uint64_t product =
        (high_total << 32u) | static_cast<std::uint32_t>(low_product);
    if (product > std::numeric_limits<std::uint64_t>::max() - kRatioRounding) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (product + kRatioRounding) >> kRatioFractionBits;
}

inline std::uint64_t transposeSemitonesFixed(
    std::uint64_t root_phase_increment_q32,
    int semitone_delta) {
    if (root_phase_increment_q32 == 0u) {
        return 0u;
    }

    int octaves = semitone_delta / 12;
    int remainder = semitone_delta % 12;
    if (remainder < 0) {
        remainder += 12;
        --octaves;
    }

    std::uint64_t value = multiplyRatioQ20(
        root_phase_increment_q32,
        kSemitoneRatioQ20[static_cast<std::size_t>(remainder)]);
    if (octaves > 0) {
        if (octaves >= 64 ||
            value > (std::numeric_limits<std::uint64_t>::max() >> octaves)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        value <<= octaves;
    } else if (octaves < 0) {
        const int shift = -octaves;
        if (shift >= 64) {
            return 1u;
        }
        value >>= shift;
    }
    return std::max<std::uint64_t>(value, 1u);
}

inline std::uint64_t transposeCentsFixed(
    std::uint64_t root_phase_increment_q32,
    int cents_delta) {
    if (root_phase_increment_q32 == 0u) {
        return 0u;
    }
    int semitones = cents_delta / 100;
    int cents = cents_delta % 100;
    if (cents < 0) {
        cents += 100;
        --semitones;
    }
    const std::uint64_t semitone_value = transposeSemitonesFixed(
        root_phase_increment_q32,
        semitones);
    return std::max<std::uint64_t>(
        multiplyRatioQ20(
            semitone_value,
            kCentRatioQ20[static_cast<std::size_t>(cents)]),
        1u);
}

}  // namespace detail

struct State {
    std::uint64_t phase_increment_q32 = kPhaseOneQ32;
    std::int32_t gain_left_q15 = kQ15Unity;
    std::int32_t gain_right_q15 = kQ15Unity;
    BiquadLowpassConfig lowpass{};
};

inline State compute(
    const VoiceStartParams& params,
    std::uint32_t vibrato_phase_q32,
    std::uint32_t vibrato_delay_remaining,
    std::uint32_t mod_lfo_phase_q32,
    std::uint32_t mod_lfo_delay_remaining,
    std::int32_t mod_envelope_dsp_level_q15) {
    State result{};
    result.phase_increment_q32 = params.phase_increment_q32;
    result.gain_left_q15 = params.gain_left_q15;
    result.gain_right_q15 = params.gain_right_q15;
    result.lowpass = params.lowpass;

    std::int64_t pitch_cents = 0;
    std::int64_t filter_offset_cents = 0;
    if (params.vibrato_lfo.enabled && vibrato_delay_remaining == 0u &&
        params.vibrato_lfo.pitch_depth_cents != 0) {
        pitch_cents += detail::scaleSignedQ15(
            params.vibrato_lfo.pitch_depth_cents,
            detail::triangleLevelQ15(vibrato_phase_q32));
    }

    if (params.mod_lfo.enabled && mod_lfo_delay_remaining == 0u) {
        const std::int32_t level_q15 = detail::triangleLevelQ15(mod_lfo_phase_q32);
        if (params.mod_lfo.pitch_depth_cents != 0) {
            pitch_cents += detail::scaleSignedQ15(
                params.mod_lfo.pitch_depth_cents,
                level_q15);
        }
        if (params.mod_lfo.filter_depth_cents != 0) {
            filter_offset_cents += detail::scaleSignedQ15(
                params.mod_lfo.filter_depth_cents,
                level_q15);
        }
        if (params.mod_lfo.gain_table.enabled) {
            const StereoGainQ15 gains = detail::interpolateGainTable(
                params.mod_lfo.gain_table,
                level_q15);
            result.gain_left_q15 = gains.left;
            result.gain_right_q15 = gains.right;
        }
    }

    if (params.mod_envelope.enabled) {
        if (params.mod_envelope.pitch_depth_cents != 0) {
            pitch_cents += detail::scaleSignedQ15(
                params.mod_envelope.pitch_depth_cents,
                mod_envelope_dsp_level_q15);
        }
        if (params.mod_envelope.filter_depth_cents != 0) {
            filter_offset_cents += detail::scaleSignedQ15(
                params.mod_envelope.filter_depth_cents,
                mod_envelope_dsp_level_q15);
        }
    }

    const std::int32_t clamped_pitch = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        pitch_cents,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
    if (clamped_pitch != 0) {
        result.phase_increment_q32 = detail::transposeCentsFixed(
            params.phase_increment_q32,
            clamped_pitch);
    }

    const std::int32_t clamped_filter_offset = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(
            filter_offset_cents,
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    if (params.dynamic_lowpass_table.enabled && clamped_filter_offset != 0) {
        result.lowpass = detail::interpolateDynamicLowpassTable(
            params.dynamic_lowpass_table,
            clamped_filter_offset);
    }
    return result;
}

}  // namespace gm_synth::voice::realtime_modulation
