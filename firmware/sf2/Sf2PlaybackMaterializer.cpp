#include "Sf2PlaybackMaterializer.h"

#include "../voice/PitchMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::uint64_t kHardIssueMask =
    PlaybackIssueInvalidSampleAddress |
    PlaybackIssueAddressOutsideResidentSample |
    PlaybackIssueInvalidLoop |
    PlaybackIssueUnsupportedSampleMode |
    PlaybackIssueRomSample |
    PlaybackIssueStereoOrLinkedSample |
    PlaybackIssueInvalidRootKey |
    PlaybackIssueInvalidPhaseIncrement |
    PlaybackIssueInvalidFixedVelocity;

std::uint32_t timecentsToFrames(std::int32_t timecents, std::uint32_t sample_rate) {
    if (sample_rate == 0) {
        return 0;
    }
    const long double seconds = std::exp2(static_cast<long double>(timecents) / 1200.0L);
    const long double frames = seconds * static_cast<long double>(sample_rate);
    if (!std::isfinite(frames) || frames <= 0.0L) {
        return 0;
    }
    const long double max_frames =
        static_cast<long double>(std::numeric_limits<std::uint32_t>::max());
    if (frames >= max_frames) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(frames + 0.5L);
}

std::int32_t unitGainQ15(double gain, std::uint64_t* issues) {
    if (!std::isfinite(gain) || gain <= 0.0) {
        return 0;
    }
    if (gain > 1.0) {
        if (issues != nullptr) {
            *issues |= PlaybackIssueGainAboveUnityClamped;
        }
        gain = 1.0;
    }
    return static_cast<std::int32_t>(
        std::lround(gain * static_cast<double>(voice::kQ15Unity)));
}

double centibelsToLinear(std::int32_t centibels) {
    return std::pow(10.0, -static_cast<double>(centibels) / 200.0);
}

bool differs(const NativeResolvedZone& zone, std::uint16_t oper, std::int32_t expected) {
    return zone.generators.value(oper, expected) != expected;
}

bool modEnvelopeNeedsDsp(const NativeResolvedZone& zone) {
    return differs(zone, 7, 0) || differs(zone, 11, 0) ||
           differs(zone, 25, -12000) || differs(zone, 26, -12000) ||
           differs(zone, 27, -12000) || differs(zone, 28, -12000) ||
           differs(zone, 29, 0) || differs(zone, 30, -12000) ||
           differs(zone, 31, 0) || differs(zone, 32, 0);
}

bool lfoNeedsDsp(const NativeResolvedZone& zone) {
    return differs(zone, 5, 0) || differs(zone, 6, 0) ||
           differs(zone, 10, 0) || differs(zone, 13, 0) ||
           differs(zone, 21, -12000) || differs(zone, 22, 0) ||
           differs(zone, 23, -12000) || differs(zone, 24, 0);
}

voice::LoopMode loopModeFor(std::int32_t sample_modes, bool* supported) {
    if (supported != nullptr) {
        *supported = true;
    }
    if ((sample_modes & ~0x3) != 0) {
        if (supported != nullptr) {
            *supported = false;
        }
        return voice::LoopMode::None;
    }
    switch (sample_modes & 0x3) {
        case 0:
            return voice::LoopMode::None;
        case 1:
            return voice::LoopMode::Continuous;
        case 3:
            return voice::LoopMode::UntilRelease;
        default:
            if (supported != nullptr) {
                *supported = false;
            }
            return voice::LoopMode::None;
    }
}

}  // namespace

PlaybackMaterializationResult Sf2PlaybackMaterializer::materialize(
    const NativeResolvedZone& native,
    gm::PlaybackZone* output) const {
    ResolvedZoneModulation conservative{};
    if (native.preset_modulator_records != 0 || native.instrument_modulator_records != 0) {
        conservative.unsupported_modulator_records = 1;
    }

    PlaybackMaterializationResult result = materialize(native, conservative, output);
    // Without the separately resolved modulator hierarchy we cannot prove that
    // generator 47 is consistently applied to every velocity-sourced custom
    // route, so retain the old conservative diagnostic for this overload.
    if (native.generators.has(47) && result.playablePreview()) {
        result.issues |= PlaybackIssueFixedVelocityPending;
        result.status = PlaybackMaterializationStatus::Preview;
    }
    return result;
}

PlaybackMaterializationResult Sf2PlaybackMaterializer::materialize(
    const NativeResolvedZone& native,
    const ResolvedZoneModulation& modulation,
    gm::PlaybackZone* output) const {
    PlaybackMaterializationResult result{};
    if (output == nullptr || output_sample_rate_ == 0) {
        result.status = PlaybackMaterializationStatus::InvalidArgument;
        return result;
    }

    // Velocity->attenuation is resolved below. Keep the remaining mandatory
    // default realtime sources explicit until channel state/pitch/LFO support
    // is wired into the runtime.
    std::uint64_t issues = PlaybackIssueDefaultModulatorsPending;
    if ((native.flags & NativeZoneFlagUnknownGenerator) != 0) {
        issues |= PlaybackIssueUnknownGenerator;
    }
    if ((native.flags & NativeZoneFlagIgnoredInvalidPresetGenerator) != 0) {
        issues |= PlaybackIssueIgnoredInvalidPresetGenerator;
    }
    if ((native.flags & NativeZoneFlagSampleAddressOutOfSmpl) != 0 ||
        native.sample_start < 0 || native.sample_start >= native.sample_end) {
        issues |= PlaybackIssueInvalidSampleAddress;
    }
    if ((native.flags & NativeZoneFlagAddressOutsideSampleHeader) != 0 ||
        native.sample_start < static_cast<std::int64_t>(native.shdr_start) ||
        native.sample_end > static_cast<std::int64_t>(native.shdr_end)) {
        issues |= PlaybackIssueAddressOutsideResidentSample;
    }
    if ((native.flags & NativeZoneFlagInvalidLoopRange) != 0) {
        issues |= PlaybackIssueInvalidLoop;
    }
    if ((native.flags & NativeZoneFlagRomSample) != 0) {
        issues |= PlaybackIssueRomSample;
    }
    if ((native.flags & NativeZoneFlagStereoOrLinkedSample) != 0) {
        issues |= PlaybackIssueStereoOrLinkedSample;
    }
    if ((native.flags & NativeZoneFlagRootKeyOutOfRange) != 0) {
        issues |= PlaybackIssueInvalidRootKey;
    }
    if (modulation.hasUnsupportedModulators()) {
        issues |= PlaybackIssueCustomModulatorsPending;
    }

    bool loop_mode_supported = false;
    const voice::LoopMode loop_mode = loopModeFor(native.sampleModes(), &loop_mode_supported);
    if (!loop_mode_supported) {
        issues |= PlaybackIssueUnsupportedSampleMode;
    }

    const std::int32_t root_key = native.rootKey();
    if (root_key < 0 || root_key > 127) {
        issues |= PlaybackIssueInvalidRootKey;
    }

    const std::int32_t scale_tuning = native.scaleTuningCentsPerKey();
    const bool has_fixed_key = native.generators.has(46);
    const std::int32_t fixed_key = native.generators.value(46);
    if (has_fixed_key && (fixed_key < 0 || fixed_key > 127)) {
        issues |= PlaybackIssueInvalidRootKey;
    }
    if (!has_fixed_key && scale_tuning != 0 && scale_tuning != 100) {
        issues |= PlaybackIssueScaleTuningPending;
    }

    const bool has_fixed_velocity = native.generators.has(47);
    const std::int32_t fixed_velocity = native.generators.value(47);
    if (has_fixed_velocity && (fixed_velocity < 0 || fixed_velocity > 127)) {
        issues |= PlaybackIssueInvalidFixedVelocity;
    }
    if (differs(native, 33, -12000)) {
        issues |= PlaybackIssueVolumeEnvelopeDelayPending;
    }
    if (differs(native, 35, -12000)) {
        issues |= PlaybackIssueVolumeEnvelopeHoldPending;
    }
    if (differs(native, 39, 0) || differs(native, 40, 0)) {
        issues |= PlaybackIssueVolumeEnvelopeKeyScalingPending;
    }
    if (differs(native, 8, 13500) || differs(native, 9, 0) ||
        differs(native, 10, 0) || differs(native, 11, 0)) {
        issues |= PlaybackIssueFilterPending;
    }
    if (modEnvelopeNeedsDsp(native)) {
        issues |= PlaybackIssueModEnvelopePending;
    }
    if (lfoNeedsDsp(native)) {
        issues |= PlaybackIssueLfoPending;
    }
    if (differs(native, 15, 0) || differs(native, 16, 0)) {
        issues |= PlaybackIssueEffectsSendPending;
    }
    if (native.panSf2Units() != 0) {
        issues |= PlaybackIssuePanLawApproximation;
    }

    gm::PlaybackZone candidate{};
    candidate.sample_id = native.sample_id;
    candidate.key_low = native.key_low;
    candidate.key_high = native.key_high;
    candidate.velocity_low = native.velocity_low;
    candidate.velocity_high = native.velocity_high;
    candidate.exclusive_class = static_cast<std::uint16_t>(native.exclusiveClass());
    candidate.root_key = root_key >= 0 && root_key <= 127
        ? static_cast<std::uint8_t>(root_key)
        : 60;
    candidate.loop_mode = loop_mode;

    const bool fixed_pitch = has_fixed_key || scale_tuning == 0;
    candidate.key_tracking = !fixed_pitch;

    long double root_cents =
        static_cast<long double>(native.coarseTuneSemitones()) * 100.0L +
        static_cast<long double>(native.fineTuneCents()) +
        static_cast<long double>(native.sample_pitch_correction_cents);
    if (has_fixed_key && root_key >= 0 && root_key <= 127 &&
        fixed_key >= 0 && fixed_key <= 127) {
        root_cents += static_cast<long double>(fixed_key - root_key) *
            static_cast<long double>(scale_tuning);
    }
    candidate.root_phase_increment_q32 = voice::phaseIncrementQ32(
        native.sample_rate,
        output_sample_rate_,
        static_cast<double>(root_cents));
    if (candidate.root_phase_increment_q32 == 0) {
        issues |= PlaybackIssueInvalidPhaseIncrement;
    }

    const bool looping = loop_mode != voice::LoopMode::None;
    const bool region_changed =
        native.sample_start != static_cast<std::int64_t>(native.shdr_start) ||
        native.sample_end != static_cast<std::int64_t>(native.shdr_end) ||
        (looping &&
         (native.loop_start != static_cast<std::int64_t>(native.shdr_loop_start) ||
          native.loop_end != static_cast<std::int64_t>(native.shdr_loop_end)));
    if (region_changed &&
        (issues & (PlaybackIssueInvalidSampleAddress |
                   PlaybackIssueAddressOutsideResidentSample)) == 0) {
        candidate.sample_region.enabled = true;
        candidate.sample_region.start_frame = static_cast<std::uint32_t>(
            native.sample_start - static_cast<std::int64_t>(native.shdr_start));
        candidate.sample_region.end_frame = static_cast<std::uint32_t>(
            native.sample_end - static_cast<std::int64_t>(native.shdr_start));
        if (looping && (issues & PlaybackIssueInvalidLoop) == 0) {
            candidate.sample_region.loop_start_frame = static_cast<std::uint32_t>(
                native.loop_start - static_cast<std::int64_t>(native.shdr_start));
            candidate.sample_region.loop_end_frame = static_cast<std::uint32_t>(
                native.loop_end - static_cast<std::int64_t>(native.shdr_start));
        }
    }

    // Retain the prepared static gains for direct/legacy PlaybackZone users.
    // SF2-generated zones additionally carry the raw note-on summing state;
    // ZoneResolver recomputes final gains from it before Voice::start().
    double static_gain = centibelsToLinear(native.initialAttenuationCb());
    if (!std::isfinite(static_gain)) {
        static_gain = 0.0;
    }
    if (static_gain > 1.0) {
        issues |= PlaybackIssueGainAboveUnityClamped;
        static_gain = 1.0;
    }

    const std::int32_t pan_units = std::max<std::int32_t>(
        -500,
        std::min<std::int32_t>(500, native.panSf2Units()));
    double left_gain = static_gain;
    double right_gain = static_gain;
    if (pan_units < 0) {
        right_gain *= static_cast<double>(500 + pan_units) / 500.0;
    } else if (pan_units > 0) {
        left_gain *= static_cast<double>(500 - pan_units) / 500.0;
    }
    candidate.gain_left_q15 = unitGainQ15(left_gain, &issues);
    candidate.gain_right_q15 = unitGainQ15(right_gain, &issues);

    candidate.note_on_gain.enabled = true;
    candidate.note_on_gain.initial_attenuation_cb = native.initialAttenuationCb();
    candidate.note_on_gain.velocity_to_attenuation_cb =
        modulation.velocity_to_attenuation_cb;
    candidate.note_on_gain.pan_sf2_units = static_cast<std::int16_t>(pan_units);
    candidate.note_on_gain.fixed_velocity = has_fixed_velocity &&
            fixed_velocity >= 0 && fixed_velocity <= 127
        ? static_cast<std::int16_t>(fixed_velocity)
        : -1;

    candidate.envelope.attack_frames = timecentsToFrames(
        native.generators.value(34, -12000),
        output_sample_rate_);
    candidate.envelope.decay_frames = timecentsToFrames(
        native.generators.value(36, -12000),
        output_sample_rate_);
    candidate.envelope.sustain_q15 = unitGainQ15(
        centibelsToLinear(native.generators.value(37, 0)),
        &issues);
    candidate.envelope.release_frames = timecentsToFrames(
        native.generators.value(38, -12000),
        output_sample_rate_);

    if ((issues & kHardIssueMask) == 0 && !candidate.valid()) {
        issues |= PlaybackIssueInvalidSampleAddress;
    }

    result.issues = issues;
    if ((issues & kHardIssueMask) != 0) {
        result.status = PlaybackMaterializationStatus::Unrepresentable;
        return result;
    }

    *output = candidate;
    result.status = issues == PlaybackIssueNone
        ? PlaybackMaterializationStatus::Qualified
        : PlaybackMaterializationStatus::Preview;
    return result;
}

}  // namespace gm_synth::sf2
