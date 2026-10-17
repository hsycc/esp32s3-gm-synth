#include "VoiceRenderer.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace gm_synth::voice {
namespace {

constexpr std::uint8_t kLfoControlFrames = 16;
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
constexpr std::uint32_t kRatioFractionBits = 20;
constexpr std::uint64_t kRatioRounding = std::uint64_t{1} << (kRatioFractionBits - 1u);

std::uint64_t multiplyRatioQ20(std::uint64_t value, std::uint32_t ratio_q20) {
    if (value == 0 || ratio_q20 == 0) {
        return 0;
    }
    const std::uint64_t max_product =
        std::numeric_limits<std::uint64_t>::max() - kRatioRounding;
    if (value > max_product / ratio_q20) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (value * ratio_q20 + kRatioRounding) >> kRatioFractionBits;
}

std::uint64_t transposeSemitonesFixed(
    std::uint64_t root_phase_increment_q32,
    int semitone_delta) {
    if (root_phase_increment_q32 == 0) {
        return 0;
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
            return 1;
        }
        value >>= shift;
    }
    return std::max<std::uint64_t>(value, 1u);
}

std::uint64_t transposeCentsFixed(
    std::uint64_t root_phase_increment_q32,
    int cents_delta) {
    if (root_phase_increment_q32 == 0) {
        return 0;
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

std::int32_t triangleLevelQ15(std::uint32_t phase_q32) {
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

std::int32_t scaleSignedQ15(std::int32_t value, std::int32_t q15) {
    const std::int64_t scaled = static_cast<std::int64_t>(value) * q15;
    if (scaled >= 0) {
        return static_cast<std::int32_t>((scaled + 16383) / 32767);
    }
    return -static_cast<std::int32_t>((-scaled + 16383) / 32767);
}

struct LfoTablePosition {
    std::size_t lower = 0;
    std::uint32_t fraction_q16 = 0;
};

LfoTablePosition lfoTablePosition(std::int32_t level_q15) {
    const std::int32_t clamped = std::clamp(level_q15, -kQ15Unity, kQ15Unity);
    const std::uint64_t normalized = static_cast<std::uint64_t>(clamped + kQ15Unity);
    constexpr std::uint64_t kSpan = static_cast<std::uint64_t>(kQ15Unity) * 2u;
    constexpr std::uint64_t kSegments = kLfoControlPointCount - 1u;
    const std::uint64_t position_q16 =
        (normalized * kSegments * 65536u + kSpan / 2u) / kSpan;
    if (position_q16 >= kSegments * 65536u) {
        return LfoTablePosition{kLfoControlPointCount - 1u, 0};
    }
    return LfoTablePosition{
        static_cast<std::size_t>(position_q16 >> 16u),
        static_cast<std::uint32_t>(position_q16 & 0xffffu),
    };
}

std::int32_t interpolateI32(
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

StereoGainQ15 interpolateGainTable(
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

BiquadLowpassConfig interpolateLowpassTable(
    const LfoLowpassTable& table,
    std::int32_t level_q15) {
    if (!table.enabled) {
        return BiquadLowpassConfig{};
    }
    const LfoTablePosition position = lfoTablePosition(level_q15);
    if (position.lower + 1u >= table.points.size()) {
        return table.points.back();
    }
    const BiquadLowpassConfig& a = table.points[position.lower];
    const BiquadLowpassConfig& b = table.points[position.lower + 1u];
    BiquadLowpassConfig result{};
    result.enabled = true;
    result.a0_q24 = interpolateI32(a.a0_q24, b.a0_q24, position.fraction_q16);
    result.a1_q24 = interpolateI32(a.a1_q24, b.a1_q24, position.fraction_q16);
    result.b1_q24 = interpolateI32(a.b1_q24, b.b1_q24, position.fraction_q16);
    result.b2_q24 = interpolateI32(a.b2_q24, b.b2_q24, position.fraction_q16);
    return result;
}

std::int32_t scaleSampleQ15(std::int32_t sample, std::int32_t gain_q15) {
    if (gain_q15 >= kQ15Unity) {
        return sample;
    }
    if (gain_q15 <= 0) {
        return 0;
    }

    const std::int64_t product = static_cast<std::int64_t>(sample) * gain_q15;
    if (product >= 0) {
        return static_cast<std::int32_t>((product + (std::int64_t{1} << 14)) >> 15);
    }
    return -static_cast<std::int32_t>(((-product) + (std::int64_t{1} << 14)) >> 15);
}

std::int32_t roundQ24ToInt(std::int64_t value) {
    constexpr std::int64_t kRound = std::int64_t{1} << (kFilterCoefficientFractionBits - 1u);
    std::int64_t rounded = 0;
    if (value >= 0) {
        rounded = (value + kRound) >> kFilterCoefficientFractionBits;
    } else {
        rounded = -(((-value) + kRound) >> kFilterCoefficientFractionBits);
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        rounded,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

}  // namespace

std::int32_t Voice::clampQ15(std::int32_t value) {
    return std::clamp(value, std::int32_t{0}, kQ15Unity);
}

std::int32_t Voice::multiplyQ15(std::int32_t a, std::int32_t b) {
    a = clampQ15(a);
    b = clampQ15(b);
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a == kQ15Unity) {
        return b;
    }
    if (b == kQ15Unity) {
        return a;
    }

    const std::int64_t product = static_cast<std::int64_t>(a) * b;
    return static_cast<std::int32_t>((product + (std::int64_t{1} << 14)) >> 15);
}

std::uint32_t Voice::playbackStartFrame() const {
    return params_.sample_region.enabled ? params_.sample_region.start_frame : 0u;
}

std::uint32_t Voice::playbackEndFrame() const {
    if (ref_.record == nullptr) {
        return 0;
    }
    return params_.sample_region.enabled
        ? params_.sample_region.end_frame
        : ref_.record->descriptor.frames;
}

std::uint32_t Voice::playbackLoopStartFrame() const {
    if (ref_.record == nullptr) {
        return 0;
    }
    return params_.sample_region.enabled
        ? params_.sample_region.loop_start_frame
        : ref_.record->descriptor.loop_start;
}

std::uint32_t Voice::playbackLoopEndFrame() const {
    if (ref_.record == nullptr) {
        return 0;
    }
    return params_.sample_region.enabled
        ? params_.sample_region.loop_end_frame
        : ref_.record->descriptor.loop_end;
}

bool Voice::validLoop() const {
    if (!ref_.valid() || ref_.record == nullptr) {
        return false;
    }
    const std::uint32_t start = playbackStartFrame();
    const std::uint32_t end = playbackEndFrame();
    const std::uint32_t loop_start = playbackLoopStartFrame();
    const std::uint32_t loop_end = playbackLoopEndFrame();
    return start <= loop_start && loop_start < loop_end && loop_end <= end;
}

bool Voice::loopActive() const {
    if (!validLoop()) {
        return false;
    }
    if (params_.loop_mode == LoopMode::Continuous) {
        return true;
    }
    return params_.loop_mode == LoopMode::UntilRelease && !key_released_;
}

bool Voice::releasing() const {
    return active_ && envelope_stage_ == EnvelopeStage::Release;
}

bool Voice::stealTail() const {
    return active_ && fast_release_kind_ == FastReleaseKind::VoiceSteal;
}

bool Voice::exclusiveTail() const {
    return active_ && fast_release_kind_ == FastReleaseKind::ExclusiveClass;
}

bool Voice::fastReleaseTail() const {
    return active_ && fast_release_kind_ != FastReleaseKind::None;
}

bool Voice::start(sample::PublishedSampleDirectory& directory, const VoiceStartParams& params) {
    if (active_) {
        return false;
    }

    auto acquired = directory.acquire(params.sample_id);
    if (!acquired.valid() || acquired.record == nullptr) {
        return false;
    }

    const auto& descriptor = acquired.record->descriptor;
    const bool descriptor_valid = descriptor.frames != 0 &&
        descriptor.pcm_bytes >= static_cast<std::size_t>(descriptor.frames) * sizeof(std::int16_t) &&
        acquired.record->pcm != nullptr;

    const std::uint32_t region_start = params.sample_region.enabled
        ? params.sample_region.start_frame
        : 0u;
    const std::uint32_t region_end = params.sample_region.enabled
        ? params.sample_region.end_frame
        : descriptor.frames;
    const std::uint32_t region_loop_start = params.sample_region.enabled
        ? params.sample_region.loop_start_frame
        : descriptor.loop_start;
    const std::uint32_t region_loop_end = params.sample_region.enabled
        ? params.sample_region.loop_end_frame
        : descriptor.loop_end;

    const bool region_valid = region_start < region_end && region_end <= descriptor.frames;
    const bool loop_valid = params.loop_mode == LoopMode::None ||
        (region_start <= region_loop_start &&
         region_loop_start < region_loop_end &&
         region_loop_end <= region_end);
    if (!descriptor_valid || !region_valid || !loop_valid ||
        params.phase_increment_q32 == 0 || !params.lowpass.valid() ||
        !params.vibrato_lfo.valid() || !params.mod_lfo.valid()) {
        directory.release(acquired);
        return false;
    }

    directory_ = &directory;
    ref_ = std::move(acquired);
    params_ = params;
    params_.gain_left_q15 = clampQ15(params.gain_left_q15);
    params_.gain_right_q15 = clampQ15(params.gain_right_q15);
    params_.envelope.sustain_q15 = clampQ15(params.envelope.sustain_q15);

    channel_ = params.channel;
    key_ = params.key;
    phase_q32_ = static_cast<std::uint64_t>(region_start) << 32;
    current_phase_increment_q32_ = params_.phase_increment_q32;
    current_gain_left_q15_ = params_.gain_left_q15;
    current_gain_right_q15_ = params_.gain_right_q15;
    current_lowpass_ = params_.lowpass;
    vibrato_phase_q32_ = 0;
    vibrato_delay_remaining_ = params_.vibrato_lfo.delay_frames;
    vibrato_control_frames_remaining_ = 0;
    mod_lfo_phase_q32_ = 0;
    mod_lfo_delay_remaining_ = params_.mod_lfo.delay_frames;
    mod_lfo_control_frames_remaining_ = 0;
    lowpass_z1_q24_ = 0;
    lowpass_z2_q24_ = 0;
    key_released_ = false;
    fast_release_kind_ = FastReleaseKind::None;
    envelope_stage_frame_ = 0;
    active_release_frames_ = 0;
    release_start_level_q15_ = 0;

    if (params_.envelope.delay_frames != 0) {
        envelope_stage_ = EnvelopeStage::Delay;
        envelope_level_q15_ = 0;
    } else if (params_.envelope.attack_frames != 0) {
        envelope_stage_ = EnvelopeStage::Attack;
        envelope_level_q15_ = 0;
    } else if (params_.envelope.hold_frames != 0) {
        envelope_stage_ = EnvelopeStage::Hold;
        envelope_level_q15_ = kQ15Unity;
    } else if (params_.envelope.decay_frames != 0) {
        envelope_stage_ = EnvelopeStage::Decay;
        envelope_level_q15_ = kQ15Unity;
    } else {
        envelope_stage_ = EnvelopeStage::Sustain;
        envelope_level_q15_ = params_.envelope.sustain_q15;
    }

    active_ = true;
    return true;
}

void Voice::beginRelease(
    std::uint32_t release_frames,
    FastReleaseKind fast_release_kind) {
    if (!active_) {
        return;
    }

    key_released_ = true;
    fast_release_kind_ = fast_release_kind;
    if (release_frames == 0 || envelope_level_q15_ <= 0) {
        finish();
        return;
    }

    active_release_frames_ = release_frames;
    release_start_level_q15_ = envelope_level_q15_;
    envelope_stage_ = EnvelopeStage::Release;
    envelope_stage_frame_ = 0;
}

void Voice::noteOff() {
    if (!active_ || key_released_) {
        return;
    }
    beginRelease(params_.envelope.release_frames, FastReleaseKind::None);
}

bool Voice::beginFastRelease(std::uint32_t fade_frames) {
    if (!active_) {
        return false;
    }
    beginRelease(
        std::max<std::uint32_t>(fade_frames, 1u),
        FastReleaseKind::VoiceSteal);
    return true;
}

bool Voice::beginExclusiveRelease(std::uint32_t fade_frames) {
    if (!active_) {
        return false;
    }
    beginRelease(
        std::max<std::uint32_t>(fade_frames, 1u),
        FastReleaseKind::ExclusiveClass);
    return true;
}

std::int32_t Voice::nextEnvelopeLevel() {
    switch (envelope_stage_) {
        case EnvelopeStage::Delay: {
            const std::uint32_t total = params_.envelope.delay_frames;
            envelope_level_q15_ = 0;
            if (total == 0 || ++envelope_stage_frame_ >= total) {
                envelope_stage_frame_ = 0;
                if (params_.envelope.attack_frames != 0) {
                    envelope_stage_ = EnvelopeStage::Attack;
                } else if (params_.envelope.hold_frames != 0) {
                    envelope_stage_ = EnvelopeStage::Hold;
                } else if (params_.envelope.decay_frames != 0) {
                    envelope_stage_ = EnvelopeStage::Decay;
                } else {
                    envelope_stage_ = EnvelopeStage::Sustain;
                }
            }
            return 0;
        }

        case EnvelopeStage::Attack: {
            const std::uint32_t total = params_.envelope.attack_frames;
            if (total == 0) {
                envelope_stage_ = params_.envelope.hold_frames != 0
                    ? EnvelopeStage::Hold
                    : (params_.envelope.decay_frames != 0
                        ? EnvelopeStage::Decay
                        : EnvelopeStage::Sustain);
                envelope_stage_frame_ = 0;
                return nextEnvelopeLevel();
            }
            const std::uint64_t numerator =
                static_cast<std::uint64_t>(kQ15Unity) * (envelope_stage_frame_ + 1u);
            envelope_level_q15_ = static_cast<std::int32_t>(numerator / total);
            ++envelope_stage_frame_;
            if (envelope_stage_frame_ >= total) {
                envelope_level_q15_ = kQ15Unity;
                envelope_stage_ = params_.envelope.hold_frames != 0
                    ? EnvelopeStage::Hold
                    : (params_.envelope.decay_frames != 0
                        ? EnvelopeStage::Decay
                        : EnvelopeStage::Sustain);
                envelope_stage_frame_ = 0;
            }
            return envelope_level_q15_;
        }

        case EnvelopeStage::Hold: {
            const std::uint32_t total = params_.envelope.hold_frames;
            envelope_level_q15_ = kQ15Unity;
            if (total == 0 || ++envelope_stage_frame_ >= total) {
                envelope_stage_frame_ = 0;
                envelope_stage_ = params_.envelope.decay_frames != 0
                    ? EnvelopeStage::Decay
                    : EnvelopeStage::Sustain;
            }
            return envelope_level_q15_;
        }

        case EnvelopeStage::Decay: {
            const std::uint32_t total = params_.envelope.decay_frames;
            if (total == 0) {
                envelope_stage_ = EnvelopeStage::Sustain;
                envelope_stage_frame_ = 0;
                envelope_level_q15_ = params_.envelope.sustain_q15;
                return envelope_level_q15_;
            }
            const std::int64_t delta =
                static_cast<std::int64_t>(params_.envelope.sustain_q15) - kQ15Unity;
            const std::int64_t position = static_cast<std::int64_t>(envelope_stage_frame_ + 1u);
            envelope_level_q15_ = clampQ15(
                static_cast<std::int32_t>(kQ15Unity + (delta * position) / total));
            ++envelope_stage_frame_;
            if (envelope_stage_frame_ >= total) {
                envelope_stage_ = EnvelopeStage::Sustain;
                envelope_stage_frame_ = 0;
                envelope_level_q15_ = params_.envelope.sustain_q15;
            }
            return envelope_level_q15_;
        }

        case EnvelopeStage::Sustain:
            envelope_level_q15_ = params_.envelope.sustain_q15;
            return envelope_level_q15_;

        case EnvelopeStage::Release: {
            const std::uint32_t total = active_release_frames_;
            if (total == 0 || envelope_stage_frame_ >= total) {
                envelope_stage_ = EnvelopeStage::Done;
                envelope_level_q15_ = 0;
                return 0;
            }
            const std::uint32_t remaining = total - envelope_stage_frame_;
            envelope_level_q15_ = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(release_start_level_q15_) * remaining) / total);
            ++envelope_stage_frame_;
            if (envelope_stage_frame_ >= total) {
                envelope_stage_ = EnvelopeStage::Done;
            }
            return envelope_level_q15_;
        }

        case EnvelopeStage::Done:
        default:
            envelope_level_q15_ = 0;
            return 0;
    }
}

std::int32_t Voice::interpolatedSample() const {
    if (!ref_.valid() || ref_.record == nullptr || ref_.record->pcm == nullptr) {
        return 0;
    }

    const std::uint32_t end_frame = playbackEndFrame();
    const std::uint32_t index = static_cast<std::uint32_t>(phase_q32_ >> 32);
    if (index >= end_frame) {
        return 0;
    }

    std::uint32_t next_index = index + 1u;
    const std::uint32_t loop_start = playbackLoopStartFrame();
    const std::uint32_t loop_end = playbackLoopEndFrame();
    if (loopActive() && index >= loop_start && next_index >= loop_end) {
        next_index = loop_start;
    } else if (next_index >= end_frame) {
        next_index = end_frame - 1u;
    }

    const std::int32_t a = ref_.record->pcm[index];
    const std::int32_t b = ref_.record->pcm[next_index];
    const std::uint32_t fraction = static_cast<std::uint32_t>(phase_q32_ & 0xffffffffu);
    const std::int64_t delta = static_cast<std::int64_t>(b) - a;
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(a) +
        ((delta * static_cast<std::int64_t>(fraction)) >> 32));
}

std::int32_t Voice::applyLowpass(std::int32_t sample) {
    if (!current_lowpass_.enabled) {
        return sample;
    }

    const BiquadLowpassConfig& filter = current_lowpass_;
    const std::int64_t output_q24 =
        static_cast<std::int64_t>(sample) * filter.a0_q24 + lowpass_z1_q24_;
    const std::int32_t output = roundQ24ToInt(output_q24);
    lowpass_z1_q24_ =
        static_cast<std::int64_t>(sample) * filter.a1_q24 +
        lowpass_z2_q24_ -
        static_cast<std::int64_t>(output) * filter.b1_q24;
    lowpass_z2_q24_ =
        static_cast<std::int64_t>(sample) * filter.a0_q24 -
        static_cast<std::int64_t>(output) * filter.b2_q24;
    return output;
}

void Voice::refreshLfoState() {
    current_phase_increment_q32_ = params_.phase_increment_q32;
    current_gain_left_q15_ = params_.gain_left_q15;
    current_gain_right_q15_ = params_.gain_right_q15;
    current_lowpass_ = params_.lowpass;

    std::int64_t pitch_cents = 0;
    if (params_.vibrato_lfo.enabled && vibrato_delay_remaining_ == 0 &&
        params_.vibrato_lfo.pitch_depth_cents != 0) {
        pitch_cents += scaleSignedQ15(
            params_.vibrato_lfo.pitch_depth_cents,
            triangleLevelQ15(vibrato_phase_q32_));
    }

    if (params_.mod_lfo.enabled && mod_lfo_delay_remaining_ == 0) {
        const std::int32_t level_q15 = triangleLevelQ15(mod_lfo_phase_q32_);
        if (params_.mod_lfo.pitch_depth_cents != 0) {
            pitch_cents += scaleSignedQ15(params_.mod_lfo.pitch_depth_cents, level_q15);
        }
        if (params_.mod_lfo.gain_table.enabled) {
            const StereoGainQ15 gains = interpolateGainTable(
                params_.mod_lfo.gain_table,
                level_q15);
            current_gain_left_q15_ = gains.left;
            current_gain_right_q15_ = gains.right;
        }
        if (params_.mod_lfo.lowpass_table.enabled) {
            current_lowpass_ = interpolateLowpassTable(
                params_.mod_lfo.lowpass_table,
                level_q15);
        }
    }

    const std::int32_t clamped_pitch = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        pitch_cents,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
    if (clamped_pitch != 0) {
        current_phase_increment_q32_ = transposeCentsFixed(
            params_.phase_increment_q32,
            clamped_pitch);
    }
}

void Voice::advanceLfos() {
    if (!active_) {
        return;
    }

    if (params_.vibrato_lfo.enabled) {
        if (vibrato_delay_remaining_ != 0) {
            --vibrato_delay_remaining_;
            if (vibrato_delay_remaining_ == 0) {
                vibrato_control_frames_remaining_ = 0;
            }
        } else {
            vibrato_phase_q32_ += params_.vibrato_lfo.phase_step_q32;
            if (vibrato_control_frames_remaining_ != 0) {
                --vibrato_control_frames_remaining_;
            }
        }
    }

    if (params_.mod_lfo.enabled) {
        if (mod_lfo_delay_remaining_ != 0) {
            --mod_lfo_delay_remaining_;
            if (mod_lfo_delay_remaining_ == 0) {
                mod_lfo_control_frames_remaining_ = 0;
            }
        } else {
            mod_lfo_phase_q32_ += params_.mod_lfo.phase_step_q32;
            if (mod_lfo_control_frames_remaining_ != 0) {
                --mod_lfo_control_frames_remaining_;
            }
        }
    }
}

void Voice::advancePhase() {
    if (!active_ || !ref_.valid() || ref_.record == nullptr) {
        return;
    }

    phase_q32_ += current_phase_increment_q32_;

    if (loopActive()) {
        const std::uint64_t start = static_cast<std::uint64_t>(playbackLoopStartFrame()) << 32;
        const std::uint64_t end = static_cast<std::uint64_t>(playbackLoopEndFrame()) << 32;
        const std::uint64_t length = end - start;
        if (phase_q32_ >= end && length != 0) {
            phase_q32_ = start + ((phase_q32_ - start) % length);
        }
        return;
    }

    if ((phase_q32_ >> 32) >= playbackEndFrame()) {
        finish();
    }
}

void Voice::finish() {
    if (ref_.valid() && directory_ != nullptr) {
        directory_->release(ref_);
    }
    active_ = false;
    key_released_ = false;
    fast_release_kind_ = FastReleaseKind::None;
    phase_q32_ = 0;
    current_phase_increment_q32_ = kPhaseOneQ32;
    current_gain_left_q15_ = kQ15Unity;
    current_gain_right_q15_ = kQ15Unity;
    current_lowpass_ = BiquadLowpassConfig{};
    vibrato_phase_q32_ = 0;
    vibrato_delay_remaining_ = 0;
    vibrato_control_frames_remaining_ = 0;
    mod_lfo_phase_q32_ = 0;
    mod_lfo_delay_remaining_ = 0;
    mod_lfo_control_frames_remaining_ = 0;
    lowpass_z1_q24_ = 0;
    lowpass_z2_q24_ = 0;
    envelope_stage_ = EnvelopeStage::Done;
    envelope_stage_frame_ = 0;
    active_release_frames_ = 0;
    envelope_level_q15_ = 0;
    release_start_level_q15_ = 0;
    directory_ = nullptr;
}

VoiceRenderResult Voice::renderAdd(
    std::int32_t* interleaved_stereo,
    std::size_t frame_count) {
    VoiceRenderResult result{};
    if (!active_ || interleaved_stereo == nullptr || frame_count == 0) {
        result.finished = !active_;
        return result;
    }

    for (std::size_t frame = 0; frame < frame_count && active_; ++frame) {
        if (!ref_.valid() || ref_.record == nullptr ||
            (phase_q32_ >> 32) >= playbackEndFrame()) {
            finish();
            break;
        }

        const bool vibrato_due = params_.vibrato_lfo.enabled &&
            vibrato_delay_remaining_ == 0 && vibrato_control_frames_remaining_ == 0;
        const bool mod_lfo_due = params_.mod_lfo.enabled &&
            mod_lfo_delay_remaining_ == 0 && mod_lfo_control_frames_remaining_ == 0;
        if (vibrato_due || mod_lfo_due) {
            refreshLfoState();
            if (params_.vibrato_lfo.enabled && vibrato_delay_remaining_ == 0) {
                vibrato_control_frames_remaining_ = kLfoControlFrames;
            }
            if (params_.mod_lfo.enabled && mod_lfo_delay_remaining_ == 0) {
                mod_lfo_control_frames_remaining_ = kLfoControlFrames;
            }
        }

        const std::int32_t envelope = nextEnvelopeLevel();
        if (envelope_stage_ == EnvelopeStage::Done && envelope <= 0) {
            finish();
            break;
        }

        const std::int32_t mono = applyLowpass(interpolatedSample());
        const std::int32_t left_gain = multiplyQ15(current_gain_left_q15_, envelope);
        const std::int32_t right_gain = multiplyQ15(current_gain_right_q15_, envelope);
        interleaved_stereo[frame * 2u] += scaleSampleQ15(mono, left_gain);
        interleaved_stereo[frame * 2u + 1u] += scaleSampleQ15(mono, right_gain);
        ++result.frames_rendered;

        const bool release_finishes_after_frame = envelope_stage_ == EnvelopeStage::Done;
        advancePhase();
        advanceLfos();
        if (release_finishes_after_frame && active_) {
            finish();
        }
    }

    result.finished = !active_;
    return result;
}

void Voice::reset() {
    finish();
}

std::size_t VoicePool::findInactiveSlot() const {
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (!voices_[index].active()) {
            return index;
        }
    }
    return kHardVoiceCap;
}

std::size_t VoicePool::chooseStealVictim() const {
    std::size_t best = kHardVoiceCap;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        const Voice& candidate = voices_[index];
        if (!candidate.active() || candidate.fastReleaseTail()) {
            continue;
        }
        if (best == kHardVoiceCap) {
            best = index;
            continue;
        }

        const Voice& incumbent = voices_[best];
        if (candidate.releasing() != incumbent.releasing()) {
            if (candidate.releasing()) {
                best = index;
            }
            continue;
        }
        if (candidate.envelopeLevelQ15() != incumbent.envelopeLevelQ15()) {
            if (candidate.envelopeLevelQ15() < incumbent.envelopeLevelQ15()) {
                best = index;
            }
            continue;
        }
        if (start_serials_[index] != start_serials_[best]) {
            if (start_serials_[index] < start_serials_[best]) {
                best = index;
            }
            continue;
        }
        if (index < best) {
            best = index;
        }
    }
    return best;
}

std::size_t VoicePool::exclusiveVictimCount(const VoiceStartParams& params) const {
    if (params.exclusive_class == 0) {
        return 0;
    }

    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (!voice.active() || voice.fastReleaseTail() ||
            voice.channel() != params.channel ||
            voice.exclusiveClass() != params.exclusive_class) {
            continue;
        }

        const bool same_note_instance = params.note_instance_id != 0 &&
            voice.noteInstanceId() == params.note_instance_id;
        if (!same_note_instance) {
            ++count;
        }
    }
    return count;
}

std::size_t VoicePool::releaseExclusiveVictims(
    const VoiceStartParams& params,
    std::size_t incoming_slot) {
    if (params.exclusive_class == 0) {
        return 0;
    }

    std::size_t released = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (index == incoming_slot) {
            continue;
        }

        Voice& voice = voices_[index];
        if (!voice.active() || voice.fastReleaseTail() ||
            voice.channel() != params.channel ||
            voice.exclusiveClass() != params.exclusive_class) {
            continue;
        }

        const bool same_note_instance = params.note_instance_id != 0 &&
            voice.noteInstanceId() == params.note_instance_id;
        if (same_note_instance) {
            continue;
        }

        if (voice.beginExclusiveRelease(steal_config_.fade_frames)) {
            ++released;
        }
    }
    return released;
}

void VoicePool::recordStartedVoice(std::size_t slot, std::size_t active_before) {
    start_serials_[slot] = next_start_serial_;
    if (next_start_serial_ != std::numeric_limits<std::uint64_t>::max()) {
        ++next_start_serial_;
    }
    util::saturatingIncrement(stats_.start_successes);
    stats_.active_voices = active_before + 1u;
    stats_.peak_active_voices = std::max(stats_.peak_active_voices, stats_.active_voices);
}

void VoicePool::refreshTailStats() {
    stats_.active_steal_tails = activeStealTailCount();
    stats_.active_exclusive_tails = activeExclusiveTailCount();
    stats_.active_fast_release_tails = activeFastReleaseTailCount();
}

bool VoicePool::start(const VoiceStartParams& params) {
    util::saturatingIncrement(stats_.start_attempts);
    const std::size_t active = activeVoiceCount();

    // Preserve the hard-cap meaning: a 33rd active Voice is never created,
    // including for semantic exclusive-class replacement.
    if (active >= kHardVoiceCap) {
        util::saturatingIncrement(stats_.start_failures_full);
        return false;
    }

    const std::size_t target = findInactiveSlot();
    if (target >= kHardVoiceCap) {
        util::saturatingIncrement(stats_.start_failures_full);
        return false;
    }

    const std::size_t exclusive_victims = exclusiveVictimCount(params);
    if (exclusive_victims != 0) {
        util::saturatingIncrement(stats_.exclusive_choke_attempts);

        const std::size_t fast_tails = activeFastReleaseTailCount();
        const std::size_t available_tail_slots =
            steal_config_.max_tail_voices > fast_tails
            ? steal_config_.max_tail_voices - fast_tails
            : 0;
        if (exclusive_victims > available_tail_slots) {
            util::saturatingIncrement(stats_.exclusive_choke_failures_tail_budget);
            util::saturatingIncrement(stats_.start_failures_admission);
            return false;
        }

        // If adaptive admission was recently lowered below the still-sounding
        // non-tail population, do not let semantic replacement add more work
        // until normal releases bring the pool back to the new target.
        const std::size_t non_tail_active = active - fast_tails;
        if (active >= admission_limit_ && non_tail_active > admission_limit_) {
            util::saturatingIncrement(stats_.exclusive_choke_failures_over_target);
            util::saturatingIncrement(stats_.start_failures_admission);
            return false;
        }
    }

    // Below the admission target, or exactly at the target with at least one
    // semantic exclusive victim, the incoming Voice may start. The latter is a
    // replacement: the old Voice(s) become bounded fast-release tails.
    if (active < admission_limit_ || exclusive_victims != 0) {
        // Acquire and validate the incoming SampleRef before changing any old
        // Voice. A missing/corrupt incoming sample must never sacrifice sound.
        if (!voices_[target].start(directory_, params)) {
            util::saturatingIncrement(stats_.start_failures_rejected);
            return false;
        }

        if (exclusive_victims != 0) {
            const std::size_t released = releaseExclusiveVictims(params, target);
            util::saturatingAdd(stats_.exclusive_voices_released, released);
            util::saturatingIncrement(stats_.exclusive_choke_successes);
            stats_.last_exclusive_class = params.exclusive_class;
        }

        recordStartedVoice(target, active);
        refreshTailStats();
        return true;
    }

    if (!steal_config_.enabled) {
        util::saturatingIncrement(stats_.start_failures_admission);
        return false;
    }

    util::saturatingIncrement(stats_.steal_attempts);
    const std::size_t fast_tails = activeFastReleaseTailCount();
    const std::size_t non_tail_active = active - fast_tails;

    // A newly lowered adaptive limit may sit below the still-sounding Voice
    // count. Do not use stealing to admit even more work until natural/normal
    // releases have brought the non-tail population back to the new target.
    if (non_tail_active > admission_limit_) {
        util::saturatingIncrement(stats_.steal_failures_over_target);
        util::saturatingIncrement(stats_.start_failures_admission);
        return false;
    }

    if (steal_config_.max_tail_voices == 0 ||
        fast_tails >= steal_config_.max_tail_voices) {
        util::saturatingIncrement(stats_.steal_failures_tail_budget);
        util::saturatingIncrement(stats_.start_failures_admission);
        return false;
    }

    const std::size_t victim = chooseStealVictim();
    if (victim >= kHardVoiceCap) {
        util::saturatingIncrement(stats_.steal_failures_no_victim);
        util::saturatingIncrement(stats_.start_failures_admission);
        return false;
    }

    // Acquire and validate the incoming SampleRef before changing the victim.
    // A rejected/missing sample must never sacrifice a currently sounding voice.
    if (!voices_[target].start(directory_, params)) {
        util::saturatingIncrement(stats_.start_failures_rejected);
        return false;
    }

    const bool victim_was_releasing = voices_[victim].releasing();
    const std::uint8_t victim_channel = voices_[victim].channel();
    const std::uint8_t victim_key = voices_[victim].key();
    if (!voices_[victim].beginFastRelease(steal_config_.fade_frames)) {
        voices_[target].reset();
        util::saturatingIncrement(stats_.steal_failures_no_victim);
        util::saturatingIncrement(stats_.start_failures_admission);
        return false;
    }

    recordStartedVoice(target, active);
    util::saturatingIncrement(stats_.steal_successes);
    if (victim_was_releasing) {
        util::saturatingIncrement(stats_.steals_from_release);
    } else {
        util::saturatingIncrement(stats_.steals_from_active);
    }
    stats_.last_stolen_channel = victim_channel;
    stats_.last_stolen_key = victim_key;
    refreshTailStats();
    return true;
}

std::size_t VoicePool::noteOff(std::uint8_t channel, std::uint8_t key) {
    std::size_t matches = 0;
    for (Voice& voice : voices_) {
        if (voice.active() && voice.channel() == channel && voice.key() == key) {
            voice.noteOff();
            ++matches;
        }
    }
    util::saturatingAdd(stats_.note_off_matches, matches);
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
    return matches;
}

void VoicePool::renderAdd(std::int32_t* interleaved_stereo, std::size_t frame_count) {
    if (interleaved_stereo == nullptr || frame_count == 0) {
        return;
    }

    for (Voice& voice : voices_) {
        if (!voice.active()) {
            continue;
        }
        const VoiceRenderResult result = voice.renderAdd(interleaved_stereo, frame_count);
        util::saturatingAdd(stats_.rendered_voice_frames, result.frames_rendered);
        if (result.finished) {
            util::saturatingIncrement(stats_.voices_finished);
        }
    }
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
}

void VoicePool::allSoundOff() {
    for (Voice& voice : voices_) {
        if (voice.active()) {
            voice.reset();
            util::saturatingIncrement(stats_.voices_finished);
        }
    }
    stats_.active_voices = 0;
    refreshTailStats();
}

void VoicePool::setAdmissionLimit(std::size_t limit) {
    admission_limit_ = std::clamp<std::size_t>(limit, 1u, kHardVoiceCap);
    stats_.admission_limit = admission_limit_;
}

void VoicePool::setVoiceStealConfig(const VoiceStealConfig& config) {
    steal_config_ = config;
    steal_config_.fade_frames = std::max<std::uint32_t>(config.fade_frames, 1u);
    steal_config_.max_tail_voices = std::min<std::size_t>(
        config.max_tail_voices,
        kHardVoiceCap);
}

std::size_t VoicePool::activeVoiceCount() const {
    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active()) {
            ++count;
        }
    }
    return count;
}

std::size_t VoicePool::activeStealTailCount() const {
    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (voice.stealTail()) {
            ++count;
        }
    }
    return count;
}

std::size_t VoicePool::activeExclusiveTailCount() const {
    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (voice.exclusiveTail()) {
            ++count;
        }
    }
    return count;
}

std::size_t VoicePool::activeFastReleaseTailCount() const {
    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (voice.fastReleaseTail()) {
            ++count;
        }
    }
    return count;
}

}  // namespace gm_synth::voice
