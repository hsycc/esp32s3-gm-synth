#include "Sf2ModulationMath.h"

#include "Sf2PanMath.h"
#include "../voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::array<std::uint16_t, 128> kConcaveQ15{{
    0, 47, 94, 142, 190, 238, 287, 336, 386, 436, 486, 537, 589, 640, 693, 745,
    798, 852, 906, 961, 1016, 1072, 1128, 1185, 1242, 1300, 1358, 1417, 1477, 1537, 1598, 1659,
    1721, 1784, 1848, 1912, 1976, 2042, 2108, 2175, 2243, 2312, 2381, 2451, 2522, 2594, 2667, 2740,
    2815, 2890, 2967, 3044, 3123, 3203, 3283, 3365, 3448, 3532, 3617, 3704, 3792, 3881, 3972, 4063,
    4157, 4252, 4348, 4446, 4546, 4647, 4750, 4855, 4962, 5071, 5182, 5295, 5410, 5527, 5647, 5769,
    5894, 6022, 6152, 6285, 6421, 6561, 6704, 6850, 7000, 7154, 7313, 7475, 7642, 7814, 7991, 8173,
    8362, 8556, 8757, 8965, 9181, 9405, 9637, 9879, 10132, 10395, 10671, 10960, 11264, 11585, 11924, 12283,
    12666, 13075, 13514, 13989, 14505, 15070, 15695, 16393, 17185, 18099, 19180, 20503, 22209, 24613, 28723, 32767,
}};

constexpr std::uint32_t kQ30One = std::uint32_t{1} << 30u;
constexpr std::uint32_t kOneCentibelGainQ30 = 1061450803u;
constexpr std::int32_t kSourceQ15One = 32767;
constexpr std::int32_t kSource127Over128Q15 = 32511;

std::uint32_t multiplyQ30(std::uint32_t a, std::uint32_t b) {
    const std::uint64_t product = static_cast<std::uint64_t>(a) * b;
    return static_cast<std::uint32_t>((product + (std::uint64_t{1} << 29u)) >> 30u);
}

std::int32_t multiplyQ15(std::int32_t a, std::int32_t b) {
    if (a <= 0 || b <= 0) {
        return 0;
    }
    if (a >= voice::kQ15Unity) {
        return std::min<std::int32_t>(b, voice::kQ15Unity);
    }
    if (b >= voice::kQ15Unity) {
        return std::min<std::int32_t>(a, voice::kQ15Unity);
    }
    const std::int64_t product = static_cast<std::int64_t>(a) * b;
    return static_cast<std::int32_t>((product + (std::int64_t{1} << 14u)) >> 15u);
}

std::int32_t divideSignedRounded(std::int64_t numerator, std::int64_t denominator) {
    if (denominator <= 0) {
        return 0;
    }
    const bool negative = numerator < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-numerator)
        : static_cast<std::uint64_t>(numerator);
    const std::uint64_t rounded =
        (magnitude + static_cast<std::uint64_t>(denominator / 2)) /
        static_cast<std::uint64_t>(denominator);
    const std::int64_t signed_value = negative
        ? -static_cast<std::int64_t>(rounded)
        : static_cast<std::int64_t>(rounded);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        signed_value,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

std::int32_t unipolarCurveQ15(std::uint8_t value, Sf2SourceCurve curve) {
    const std::size_t index = std::min<std::size_t>(value, 127u);
    switch (curve) {
        case Sf2SourceCurve::Linear:
            return static_cast<std::int32_t>(
                (static_cast<std::int64_t>(index) * kSourceQ15One + 64) / 128);
        case Sf2SourceCurve::Concave:
            return std::min<std::int32_t>(kConcaveQ15[index], kSource127Over128Q15);
        case Sf2SourceCurve::Convex: {
            const std::size_t mirror = 127u - index;
            const std::int32_t mirrored_concave = std::min<std::int32_t>(
                kConcaveQ15[mirror],
                kSource127Over128Q15);
            return kSource127Over128Q15 - mirrored_concave;
        }
        case Sf2SourceCurve::Switch:
            return index >= 64u ? kSource127Over128Q15 : 0;
    }
    return 0;
}

std::int32_t nonlinearSourceQ15(
    const Sf2SourceDescriptor& source,
    std::uint8_t input) {
    const std::uint8_t clamped = static_cast<std::uint8_t>(
        std::min<unsigned>(input, 127u));
    if (!source.bipolar) {
        if (!source.negative) {
            return unipolarCurveQ15(clamped, source.curve);
        }
        if (clamped == 0u) {
            return kSourceQ15One;
        }
        if (clamped >= 127u) {
            return 0;
        }
        const std::uint8_t reversed = static_cast<std::uint8_t>(128u - clamped);
        return unipolarCurveQ15(reversed, source.curve);
    }

    std::int32_t bipolar_numerator = 2 * static_cast<std::int32_t>(clamped) - 128;
    if (source.negative) {
        bipolar_numerator = -bipolar_numerator;
    }
    if (source.curve == Sf2SourceCurve::Switch) {
        return bipolar_numerator >= 0 ? kSourceQ15One : -kSourceQ15One;
    }

    const bool negative = bipolar_numerator < 0;
    const std::int32_t magnitude = negative ? -bipolar_numerator : bipolar_numerator;
    const std::int32_t mapped = magnitude >= 128
        ? kSourceQ15One
        : unipolarCurveQ15(static_cast<std::uint8_t>(magnitude), source.curve);
    return negative ? -mapped : mapped;
}

void attenuationAndPanToGainQ15(
    std::int64_t attenuation_cb,
    std::int64_t pan_sf2_units,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15) {
    const std::int32_t clamped_attenuation = static_cast<std::int32_t>(
        std::max<std::int64_t>(0, std::min<std::int64_t>(kSf2MaxAttenuationCb, attenuation_cb)));
    const std::int32_t amplitude_q15 = attenuationCbToGainQ15(clamped_attenuation);
    const std::int32_t pan = static_cast<std::int32_t>(
        std::max<std::int64_t>(-500, std::min<std::int64_t>(500, pan_sf2_units)));

    *gain_left_q15 = multiplyQ15(amplitude_q15, sf2PanChannelGainQ15(pan, true));
    *gain_right_q15 = multiplyQ15(amplitude_q15, sf2PanChannelGainQ15(pan, false));
}

}  // namespace

std::int32_t noteOnSourceToAmount(
    std::uint16_t source_oper,
    std::uint8_t key,
    std::uint8_t velocity,
    std::int32_t amount) {
    Sf2SourceDescriptor source{};
    if (!isSupportedSf2NoteOnSourceOper(source_oper) ||
        !decodeSf2SourceOper(source_oper, &source) || amount == 0) {
        return 0;
    }

    const std::uint8_t input = source.index == 2u ? velocity : key;
    const std::int32_t clamped = static_cast<std::int32_t>(
        std::min<unsigned>(input, 127u));

    // Keep linear routes exactly on the same 128-step integer model already
    // used by positiveLinearToAmount(), velocityToFilterCutoffCents() and CC10.
    if (source.curve == Sf2SourceCurve::Linear) {
        if (source.bipolar) {
            std::int32_t numerator = 2 * clamped - 128;
            if (source.negative) {
                numerator = -numerator;
            }
            return divideSignedRounded(
                static_cast<std::int64_t>(amount) * numerator,
                128);
        }

        std::int32_t factor = clamped;
        if (source.negative) {
            factor = clamped >= 127 ? 0 : 128 - clamped;
        }
        return divideSignedRounded(
            static_cast<std::int64_t>(amount) * factor,
            128);
    }

    return divideSignedRounded(
        static_cast<std::int64_t>(amount) * nonlinearSourceQ15(source, input),
        kSourceQ15One);
}

std::int32_t negativeConcaveToAmount(std::uint8_t value, std::int32_t amount) {
    const unsigned clamped = std::min<unsigned>(value, 127u);
    const std::size_t curve_index = 127u - clamped;
    const std::int64_t scaled =
        static_cast<std::int64_t>(amount) * kConcaveQ15[curve_index];
    if (scaled >= 0) {
        return static_cast<std::int32_t>((scaled + 16383) / 32767);
    }
    return -static_cast<std::int32_t>((-scaled + 16383) / 32767);
}

std::int32_t positiveLinearToAmount(std::uint8_t value, std::int32_t amount) {
    const std::int64_t scaled =
        static_cast<std::int64_t>(std::min<unsigned>(value, 127u)) * amount;
    if (scaled >= 0) {
        return static_cast<std::int32_t>((scaled + 64) / 128);
    }
    return -static_cast<std::int32_t>((-scaled + 64) / 128);
}

std::int32_t velocityToAttenuationCb(std::uint8_t velocity, std::int32_t amount_cb) {
    return negativeConcaveToAmount(velocity, amount_cb);
}

std::int32_t controllerToAttenuationCb(std::uint8_t value, std::int32_t amount_cb) {
    return negativeConcaveToAmount(value, amount_cb);
}

std::int32_t controllerToPanSf2Units(std::uint8_t value, std::int32_t amount) {
    const std::int64_t bipolar_numerator =
        static_cast<std::int64_t>(2 * std::min<unsigned>(value, 127u)) - 128;
    const std::int64_t scaled = bipolar_numerator * amount;
    if (scaled >= 0) {
        return static_cast<std::int32_t>((scaled + 64) / 128);
    }
    return -static_cast<std::int32_t>((-scaled + 64) / 128);
}

std::int32_t controllerToEffectsSendSf2Units(std::uint8_t value, std::int32_t amount) {
    return positiveLinearToAmount(value, amount);
}

std::int32_t channelEffectsSendSf2Units(
    std::int32_t static_send_sf2_units,
    std::uint8_t controller_value,
    std::int32_t controller_amount) {
    const std::int64_t summed = static_cast<std::int64_t>(static_send_sf2_units) +
        controllerToEffectsSendSf2Units(controller_value, controller_amount);
    return static_cast<std::int32_t>(
        std::max<std::int64_t>(0, std::min<std::int64_t>(kSf2MaxEffectsSend, summed)));
}

std::int32_t attenuationCbToGainQ15(std::int32_t attenuation_cb) {
    if (attenuation_cb <= 0) {
        return voice::kQ15Unity;
    }

    std::uint32_t exponent = static_cast<std::uint32_t>(std::min<std::int32_t>(
        attenuation_cb,
        kSf2MaxAttenuationCb));
    std::uint32_t result_q30 = kQ30One;
    std::uint32_t base_q30 = kOneCentibelGainQ30;
    while (exponent != 0) {
        if ((exponent & 1u) != 0) {
            result_q30 = multiplyQ30(result_q30, base_q30);
        }
        exponent >>= 1u;
        if (exponent != 0) {
            base_q30 = multiplyQ30(base_q30, base_q30);
        }
    }

    const std::uint64_t q15 =
        static_cast<std::uint64_t>(result_q30) * voice::kQ15Unity +
        (std::uint64_t{1} << 29u);
    return static_cast<std::int32_t>(q15 >> 30u);
}

void noteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t pan_sf2_units,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15) {
    if (gain_left_q15 == nullptr || gain_right_q15 == nullptr) {
        return;
    }

    const std::int64_t attenuation = static_cast<std::int64_t>(initial_attenuation_cb) +
        velocityToAttenuationCb(effective_velocity, velocity_to_attenuation_cb);
    attenuationAndPanToGainQ15(
        attenuation,
        pan_sf2_units,
        gain_left_q15,
        gain_right_q15);
}

void channelNoteOnGainQ15(
    std::int32_t initial_attenuation_cb,
    std::int32_t velocity_to_attenuation_cb,
    std::uint8_t effective_velocity,
    std::int32_t zone_pan_sf2_units,
    std::uint8_t channel_volume,
    std::int32_t volume_to_attenuation_cb,
    std::uint8_t channel_pan,
    std::int32_t pan_amount,
    std::uint8_t channel_expression,
    std::int32_t expression_to_attenuation_cb,
    std::int32_t* gain_left_q15,
    std::int32_t* gain_right_q15) {
    if (gain_left_q15 == nullptr || gain_right_q15 == nullptr) {
        return;
    }

    const std::int64_t attenuation =
        static_cast<std::int64_t>(initial_attenuation_cb) +
        velocityToAttenuationCb(effective_velocity, velocity_to_attenuation_cb) +
        controllerToAttenuationCb(channel_volume, volume_to_attenuation_cb) +
        controllerToAttenuationCb(channel_expression, expression_to_attenuation_cb);
    const std::int64_t pan = static_cast<std::int64_t>(zone_pan_sf2_units) +
        controllerToPanSf2Units(channel_pan, pan_amount);
    attenuationAndPanToGainQ15(
        attenuation,
        pan,
        gain_left_q15,
        gain_right_q15);
}

}  // namespace gm_synth::sf2
