#pragma once

#include "Sf2ResolvedZone.h"
#include "Sf2ZoneModulatorResolver.h"
#include "../gm/PlaybackProgramSnapshot.h"

#include <cstdint>

namespace gm_synth::sf2 {

enum class PlaybackMaterializationStatus : std::uint8_t {
    Qualified = 0,
    Preview,
    Unrepresentable,
    InvalidArgument,
};

enum PlaybackMaterializationIssue : std::uint64_t {
    PlaybackIssueNone = 0,

    // Reserved historical diagnostic bit. The mandatory runtime routes currently
    // targeted for V1 (velocity attenuation/filter, Channel Pressure/CC1
    // vibrato, CC7/10/11/91/93 and Pitch Wheel/RPN) are represented now. Keep
    // the bit position stable for old telemetry decoders, but do not emit it.
    PlaybackIssueDefaultModulatorsPending = std::uint64_t{1} << 0,
    PlaybackIssueCustomModulatorsPending = std::uint64_t{1} << 1,
    PlaybackIssueUnknownGenerator = std::uint64_t{1} << 2,

    // These cannot be represented by the current one-resident-sample/one-Voice
    // playback contract without changing residency or voice topology.
    PlaybackIssueInvalidSampleAddress = std::uint64_t{1} << 3,
    PlaybackIssueAddressOutsideResidentSample = std::uint64_t{1} << 4,
    PlaybackIssueInvalidLoop = std::uint64_t{1} << 5,
    PlaybackIssueUnsupportedSampleMode = std::uint64_t{1} << 6,
    PlaybackIssueRomSample = std::uint64_t{1} << 7,
    PlaybackIssueStereoOrLinkedSample = std::uint64_t{1} << 8,
    PlaybackIssueInvalidRootKey = std::uint64_t{1} << 9,
    PlaybackIssueInvalidPhaseIncrement = std::uint64_t{1} << 10,

    // Preserved native semantics which are not fully implemented yet.
    PlaybackIssueScaleTuningPending = std::uint64_t{1} << 11,
    // Reserved historical diagnostic bits. Volume-envelope delay/hold and their
    // key scaling are implemented now; these bits are intentionally no longer
    // emitted so old telemetry decoders keep stable bit positions.
    PlaybackIssueVolumeEnvelopeDelayPending = std::uint64_t{1} << 12,
    PlaybackIssueVolumeEnvelopeHoldPending = std::uint64_t{1} << 13,
    PlaybackIssueVolumeEnvelopeKeyScalingPending = std::uint64_t{1} << 14,
    // Static cutoff, velocity, resonance, Mod-LFO and Mod-Envelope cutoff
    // movement are represented at 44.1 kHz. This bit is retained for output
    // sample rates whose filter coefficient path is not yet qualified.
    PlaybackIssueFilterPending = std::uint64_t{1} << 15,
    // Reserved historical diagnostic bit. SF2 modulation-envelope timing,
    // key-scaled hold/decay and pitch/filter destinations are implemented now.
    PlaybackIssueModEnvelopePending = std::uint64_t{1} << 16,
    // Reserved historical diagnostic bit. Vibrato LFO and Mod-LFO pitch/filter/
    // volume destinations are implemented now; keep this bit stable but unused.
    PlaybackIssueLfoPending = std::uint64_t{1} << 17,
    // Reverb/chorus generator + CC91/93 send values are preserved and updated
    // sample-accurately in Voice state; this bit means the actual FX bus/DSP
    // processor that consumes those sends is still pending.
    PlaybackIssueEffectsSendPending = std::uint64_t{1} << 18,
    // Retained as a diagnostics bit for callers using the conservative legacy
    // materialize(native, output) overload without per-zone modulator state.
    PlaybackIssueFixedVelocityPending = std::uint64_t{1} << 19,

    // Static pan is materialized with the current lightweight linear balance
    // law. Keep the approximation explicit until the final SF2 pan law is used.
    PlaybackIssuePanLawApproximation = std::uint64_t{1} << 20,
    PlaybackIssueGainAboveUnityClamped = std::uint64_t{1} << 21,
    PlaybackIssueIgnoredInvalidPresetGenerator = std::uint64_t{1} << 22,
    PlaybackIssueInvalidFixedVelocity = std::uint64_t{1} << 23,
};

struct PlaybackMaterializationResult {
    PlaybackMaterializationStatus status = PlaybackMaterializationStatus::InvalidArgument;
    std::uint64_t issues = PlaybackIssueNone;

    bool hasIssue(PlaybackMaterializationIssue issue) const {
        return (issues & static_cast<std::uint64_t>(issue)) != 0;
    }
    bool playablePreview() const {
        return status == PlaybackMaterializationStatus::Qualified ||
               status == PlaybackMaterializationStatus::Preview;
    }
};

// Converts one native SF2 zone into the compact realtime PlaybackZone supported
// by the current engine. The conversion is transactional: on Unrepresentable or
// InvalidArgument the caller's output is left unchanged.
class Sf2PlaybackMaterializer {
public:
    explicit Sf2PlaybackMaterializer(std::uint32_t output_sample_rate = 44100)
        : output_sample_rate_(output_sample_rate) {}

    // Conservative compatibility overload. It assumes file-defined modulators
    // are unsupported because no resolved route information was supplied.
    PlaybackMaterializationResult materialize(
        const NativeResolvedZone& native,
        gm::PlaybackZone* output) const;

    PlaybackMaterializationResult materialize(
        const NativeResolvedZone& native,
        const ResolvedZoneModulation& modulation,
        gm::PlaybackZone* output) const;

private:
    std::uint32_t output_sample_rate_ = 44100;
};

}  // namespace gm_synth::sf2
