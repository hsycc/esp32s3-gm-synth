#pragma once

#include "Sf2ResolvedZone.h"
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

    // The current engine has not implemented the mandatory SF2 default
    // modulator set yet. A generator-only zone can therefore be previewed, but
    // must not be advertised as fully SoundFont-qualified.
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

    // These are preserved in NativeResolvedZone but the current Voice DSP does
    // not yet implement their exact semantics.
    PlaybackIssueScaleTuningPending = std::uint64_t{1} << 11,
    PlaybackIssueVolumeEnvelopeDelayPending = std::uint64_t{1} << 12,
    PlaybackIssueVolumeEnvelopeHoldPending = std::uint64_t{1} << 13,
    PlaybackIssueVolumeEnvelopeKeyScalingPending = std::uint64_t{1} << 14,
    PlaybackIssueFilterPending = std::uint64_t{1} << 15,
    PlaybackIssueModEnvelopePending = std::uint64_t{1} << 16,
    PlaybackIssueLfoPending = std::uint64_t{1} << 17,
    PlaybackIssueEffectsSendPending = std::uint64_t{1} << 18,
    PlaybackIssueFixedVelocityPending = std::uint64_t{1} << 19,

    // Static pan is materialized with the current lightweight linear balance
    // law. Keep the approximation explicit until the final SF2 pan law and
    // channel-controller composition are implemented together.
    PlaybackIssuePanLawApproximation = std::uint64_t{1} << 20,
    PlaybackIssueGainAboveUnityClamped = std::uint64_t{1} << 21,
    PlaybackIssueIgnoredInvalidPresetGenerator = std::uint64_t{1} << 22,
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

    PlaybackMaterializationResult materialize(
        const NativeResolvedZone& native,
        gm::PlaybackZone* output) const;

private:
    std::uint32_t output_sample_rate_ = 44100;
};

}  // namespace gm_synth::sf2
