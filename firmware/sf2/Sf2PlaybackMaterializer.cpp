#include "Sf2PlaybackMaterializer.h"

#include "Sf2FilterMath.h"
#include "Sf2ModulationMath.h"
#include "Sf2ResonantFilterMath.h"
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

std::uint32_t lfoFrequencyCentsToPhaseStepQ32(
    std::int32_t frequency_cents,
    std::uint32_t sample_rate) {
    if (sample_rate == 0) {
        return 0;
    }
    constexpr long double kAbsoluteCentsZeroHz = 8.176L;
    constexpr long double kQ32 = 4294967296.0L;
    const long double hz = kAbsoluteCentsZeroHz *
        std::exp2(static_cast<long double>(frequency_cents) / 1200.0L);
    const long double step = hz * kQ32 / static_cast<long double>(sample_rate);
    if (!std::isfinite(step) || step <= 0.0L) {
        return 1;
    }
    if (step >= static_cast<long double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(step + 0.5L));
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

std::int32_t modulationEnvelopeSustainQ15(std::int32_t sustain_units) {
    const std::int32_t clamped = std::clamp<std::int32_t>(sustain_units, 0, 1000);
    const std::int64_t numerator =
        static_cast<std::int64_t>(1000 - clamped) * voice::kQ15Unity + 500;
    return static_cast<std::int32_t>(numerator / 1000);
}

bool differs(const NativeResolvedZone& zone, std::uint16_t oper, std::int32_t expected) {
    return zone.generators.value(oper, expected) != expected;
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

    // Mandatory SF2 defaults currently represented by the runtime: NoteOn
    // velocity -> attenuation/filter, Channel Pressure/CC1 -> vibrato depth,
    // CC7/10/11, Pitch Wheel/RPN and CC91/93. Remaining preview diagnostics
    // below therefore describe non-default DSP/features rather than a blanket
    // default-modulator gap.
    std::uint64_t issues = PlaybackIssueNone;
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

    const std::int32_t raw_scale_tuning = native.scaleTuningCentsPerKey();
    const std::int32_t scale_tuning = std::clamp<std::int32_t>(raw_scale_tuning, 0, 1200);
    if (raw_scale_tuning != scale_tuning) {
        // Keep the historical bit useful for malformed/out-of-range files while
        // fully qualifying every valid SF2 scaleTuning value.
        issues |= PlaybackIssueScaleTuningPending;
    }
    const bool has_fixed_key = native.generators.has(46);
    const std::int32_t fixed_key = native.generators.value(46);
    if (has_fixed_key && (fixed_key < 0 || fixed_key > 127)) {
        issues |= PlaybackIssueInvalidRootKey;
    }

    const bool has_fixed_velocity = native.generators.has(47);
    const std::int32_t fixed_velocity = native.generators.value(47);
    if (has_fixed_velocity && (fixed_velocity < 0 || fixed_velocity > 127)) {
        issues |= PlaybackIssueInvalidFixedVelocity;
    }

    // The current fixed-point filter path covers static cutoff, velocity,
    // resonance, Mod-LFO and Mod-Envelope movement at 44.1 kHz.
    if (output_sample_rate_ != kSf2FilterOutputSampleRate) {
        issues |= PlaybackIssueFilterPending;
    }
    // Both SoundFont LFOs and the modulation envelope are represented. Their
    // historical issue bits remain reserved for telemetry compatibility only.
    // The send generators/controller routes are preserved and updated at runtime,
    // but no reverb/chorus processor consumes the sends yet.
    if (differs(native, 15, 0) || differs(native, 16, 0) ||
        modulation.reverb_send_amount != 0 || modulation.chorus_send_amount != 0) {
        issues |= PlaybackIssueEffectsSendPending;
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
    candidate.scale_tuning_cents_per_key = static_cast<std::uint16_t>(scale_tuning);

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

    // Retain prepared static gains for direct/legacy PlaybackZone users. The
    // same fixed-point attenuation + equal-power pan path is used by NoteOn so
    // static and controller-driven playback cannot disagree about pan law.
    const std::int32_t static_attenuation_cb = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(
            static_cast<std::int64_t>(native.initialAttenuationCb()) +
                resonanceDcAttenuationCb(native.initialFilterQCb()),
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    if (static_attenuation_cb < 0) {
        issues |= PlaybackIssueGainAboveUnityClamped;
    }

    const std::int32_t pan_units = std::max<std::int32_t>(
        -500,
        std::min<std::int32_t>(500, native.panSf2Units()));
    noteOnGainQ15(
        static_attenuation_cb,
        0,
        127,
        pan_units,
        &candidate.gain_left_q15,
        &candidate.gain_right_q15);

    candidate.note_on_gain.enabled = true;
    candidate.note_on_gain.initial_attenuation_cb = native.initialAttenuationCb();
    candidate.note_on_gain.velocity_to_attenuation_cb = modulation.velocity_to_attenuation_cb;
    candidate.note_on_gain.volume_to_attenuation_cb = modulation.volume_to_attenuation_cb;
    candidate.note_on_gain.pan_amount = modulation.pan_amount;
    candidate.note_on_gain.expression_to_attenuation_cb =
        modulation.expression_to_attenuation_cb;
    candidate.note_on_gain.pan_sf2_units = static_cast<std::int16_t>(pan_units);
    candidate.note_on_gain.fixed_velocity = has_fixed_velocity &&
            fixed_velocity >= 0 && fixed_velocity <= 127
        ? static_cast<std::int16_t>(fixed_velocity)
        : -1;

    candidate.note_on_filter.enabled = true;
    candidate.note_on_filter.initial_cutoff_cents = std::clamp<std::int32_t>(
        native.initialFilterFcCents(),
        kSf2MinFilterCutoffCents,
        kSf2MaxFilterCutoffCents);
    candidate.note_on_filter.velocity_to_cutoff_cents =
        modulation.velocity_to_filter_fc_cents;
    candidate.note_on_filter.resonance_cb = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(native.initialFilterQCb(), 0, 960));

    const std::int32_t static_vibrato_depth = native.generators.value(6, 0);
    candidate.vibrato_lfo.enabled =
        static_vibrato_depth != 0 ||
        modulation.channel_pressure_to_vib_lfo_pitch_cents != 0 ||
        modulation.modulation_wheel_to_vib_lfo_pitch_cents != 0;
    candidate.vibrato_lfo.delay_frames = timecentsToFrames(
        native.generators.value(23, -12000),
        output_sample_rate_);
    candidate.vibrato_lfo.phase_step_q32 = lfoFrequencyCentsToPhaseStepQ32(
        native.generators.value(24, 0),
        output_sample_rate_);
    candidate.vibrato_lfo.static_pitch_depth_cents = static_vibrato_depth;
    candidate.vibrato_lfo.channel_pressure_to_pitch_cents =
        modulation.channel_pressure_to_vib_lfo_pitch_cents;
    candidate.vibrato_lfo.modulation_wheel_to_pitch_cents =
        modulation.modulation_wheel_to_vib_lfo_pitch_cents;

    candidate.mod_lfo.pitch_depth_cents = std::clamp<std::int32_t>(
        native.generators.value(5, 0), -12000, 12000);
    candidate.mod_lfo.filter_depth_cents = std::clamp<std::int32_t>(
        native.generators.value(10, 0), -12000, 12000);
    candidate.mod_lfo.volume_depth_cb = std::clamp<std::int32_t>(
        native.generators.value(13, 0), -960, 960);
    candidate.mod_lfo.enabled = candidate.mod_lfo.pitch_depth_cents != 0 ||
        candidate.mod_lfo.filter_depth_cents != 0 ||
        candidate.mod_lfo.volume_depth_cb != 0;
    candidate.mod_lfo.delay_frames = timecentsToFrames(
        native.generators.value(21, -12000),
        output_sample_rate_);
    candidate.mod_lfo.phase_step_q32 = lfoFrequencyCentsToPhaseStepQ32(
        native.generators.value(22, 0),
        output_sample_rate_);

    candidate.mod_envelope.pitch_depth_cents = std::clamp<std::int32_t>(
        native.generators.value(7, 0), -12000, 12000);
    candidate.mod_envelope.filter_depth_cents = std::clamp<std::int32_t>(
        native.generators.value(11, 0), -12000, 12000);
    candidate.mod_envelope.enabled = candidate.mod_envelope.pitch_depth_cents != 0 ||
        candidate.mod_envelope.filter_depth_cents != 0;
    candidate.mod_envelope.envelope.delay_frames = timecentsToFrames(
        native.generators.value(25, -12000),
        output_sample_rate_);
    candidate.mod_envelope.envelope.attack_frames = timecentsToFrames(
        native.generators.value(26, -12000),
        output_sample_rate_);
    candidate.mod_envelope.envelope.hold_frames = timecentsToFrames(
        native.generators.value(27, -12000),
        output_sample_rate_);
    candidate.mod_envelope.envelope.decay_frames = timecentsToFrames(
        native.generators.value(28, -12000),
        output_sample_rate_);
    candidate.mod_envelope.envelope.sustain_q15 = modulationEnvelopeSustainQ15(
        native.generators.value(29, 0));
    candidate.mod_envelope.envelope.release_frames = timecentsToFrames(
        native.generators.value(30, -12000),
        output_sample_rate_);
    candidate.mod_envelope.key_scaling.hold_timecents_per_key =
        static_cast<std::int16_t>(std::clamp<std::int32_t>(
            native.generators.value(31, 0), -1200, 1200));
    candidate.mod_envelope.key_scaling.decay_timecents_per_key =
        static_cast<std::int16_t>(std::clamp<std::int32_t>(
            native.generators.value(32, 0), -1200, 1200));

    candidate.effects_send.enabled = true;
    candidate.effects_send.reverb_sf2_units = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(native.generators.value(16, 0), 0, 1000));
    candidate.effects_send.chorus_sf2_units = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(native.generators.value(15, 0), 0, 1000));
    candidate.effects_send.reverb_cc_amount = modulation.reverb_send_amount;
    candidate.effects_send.chorus_cc_amount = modulation.chorus_send_amount;

    candidate.envelope.delay_frames = timecentsToFrames(
        native.generators.value(33, -12000),
        output_sample_rate_);
    candidate.envelope.attack_frames = timecentsToFrames(
        native.generators.value(34, -12000),
        output_sample_rate_);
    candidate.envelope.hold_frames = timecentsToFrames(
        native.generators.value(35, -12000),
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
    candidate.envelope_key_scaling.hold_timecents_per_key = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(native.generators.value(39, 0), -1200, 1200));
    candidate.envelope_key_scaling.decay_timecents_per_key = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(native.generators.value(40, 0), -1200, 1200));

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
