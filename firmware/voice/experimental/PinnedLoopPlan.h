#pragma once

#include "BoundedLoopReleaseCursor.h"
#include "sample/PublishedSampleDirectory.h"
#include <array>
#include <utility>

namespace gm_synth::voice::experimental {

// Ephemeral, resident-relative identity. The trusted loader must supply current
// context independently of a proposal, after verifying the asset and offsets.
// This is NOT a serialized cache format or a cryptographic approval mechanism.
struct LoopPlanContext {
    std::array<std::uint8_t, 32> asset_sha256{};
    std::uint64_t session_serial = 0;
    std::uint32_t bank_generation = 0, sample_generation = 0;
    sample::SampleId sample_id = 0;
    std::uint32_t sample_frames = 0, sample_rate = 0;
    CursorRegion region{};
    CursorLoopMode mode = CursorLoopMode::None;
    std::uint32_t policy_revision = 3, coordinate_revision = 1;
    std::uint16_t sample_type = 1;
    bool noteon_addresses_fixed = false;
};

inline bool sameContext(const LoopPlanContext& a, const LoopPlanContext& b) noexcept {
    return a.asset_sha256 == b.asset_sha256 && a.session_serial == b.session_serial &&
        a.bank_generation == b.bank_generation && a.sample_generation == b.sample_generation &&
        a.sample_id == b.sample_id && a.sample_frames == b.sample_frames &&
        a.sample_rate == b.sample_rate && a.region.start == b.region.start &&
        a.region.end == b.region.end && a.region.loop_start == b.region.loop_start &&
        a.region.loop_end == b.region.loop_end && a.mode == b.mode &&
        a.policy_revision == b.policy_revision && a.coordinate_revision == b.coordinate_revision &&
        a.sample_type == b.sample_type && a.noteon_addresses_fixed == b.noteon_addresses_fixed;
}

struct LoopPlanRequest {
    LoopPlanContext context{};
    bool experimental_opt_in = false;
    ExperimentalFade fade{};
    ExperimentalReleaseBridge release{};
};

enum class LoopBindingStatus : std::uint8_t {
    Ok, Busy, OptInRequired, InvalidContext, ContextMismatch, InvalidConfiguration,
    SampleUnavailable, RecordMismatch, MissingPlan, WrongDirectory, StalePublication
};

class PinnedLoopPlayback;

// Prepare/control-side owner of one directory ref, not a new PCM allocation.
// Immutable while bound. Publish only through an externally synchronized
// handoff; bind/reset and playback.start must NEVER race on the same plan.
// Directory/record owner must outlive all plans and playbacks, obey existing
// unpublish -> canReclaim rules, and never mutate pinned record/PCM fields.
class PinnedLoopPlan {
public:
    PinnedLoopPlan() = default;
    ~PinnedLoopPlan() { (void)reset(); }
    PinnedLoopPlan(const PinnedLoopPlan&) = delete;
    PinnedLoopPlan& operator=(const PinnedLoopPlan&) = delete;
    PinnedLoopPlan(PinnedLoopPlan&&) = delete;
    PinnedLoopPlan& operator=(PinnedLoopPlan&&) = delete;

    bool valid() const noexcept { return ref_.valid(); }
    LoopPlanContext context() const noexcept { return context_; } // value, never mutable alias
    ExperimentalFade fade() const noexcept { return fade_; }
    ExperimentalReleaseBridge releaseBridge() const noexcept { return release_; }

    LoopBindingStatus bind(sample::PublishedSampleDirectory& directory,
                           const LoopPlanRequest& proposal,
                           const LoopPlanContext& current) noexcept {
        if (valid()) return LoopBindingStatus::Busy;
        if (!proposal.experimental_opt_in) return LoopBindingStatus::OptInRequired;
        if (!validContext(current) || !validContext(proposal.context))
            return LoopBindingStatus::InvalidContext;
        if (!sameContext(proposal.context, current)) return LoopBindingStatus::ContextMismatch;
        if ((!proposal.fade.enabled && proposal.fade.frames != 0) ||
            (!proposal.release.enabled && proposal.release.output_frames != 0) ||
            (proposal.release.enabled && !proposal.fade.enabled))
            return LoopBindingStatus::InvalidConfiguration;
        auto acquired = directory.acquire(current.sample_id);
        if (!acquired.valid()) return LoopBindingStatus::SampleUnavailable;
        if (!recordMatches(acquired, current)) {
            (void)directory.release(acquired);
            return LoopBindingStatus::RecordMismatch;
        }
        BoundedLoopReleaseCursor check;
        if (!check.start(acquired.record->pcm, current.sample_frames, current.region,
                         current.mode, LoopCrossfadeCursor::kOne, proposal.fade, proposal.release)) {
            (void)directory.release(acquired);
            return LoopBindingStatus::InvalidConfiguration;
        }
        // Only commit after every check; failure never overwrites a live pin.
        check.reset();
        ref_ = std::move(acquired);
        directory_ = &directory;
        context_ = current;
        fade_ = proposal.fade;
        release_ = proposal.release;
        return LoopBindingStatus::Ok;
    }

    // BEGIN V1_VOICE_BINDING
    // Synchronous production-Voice binding to its independently acquired ref.
    // The immutable plan must be protected by the same handoff as start(). No
    // pointer or ownership is exported; a matching generation alone is NOT
    // sufficient if the current resident pointer has changed.
    LoopBindingStatus validateVoiceAcquisition(
        const sample::PublishedSampleDirectory& directory,
        const sample::PublishedVoiceRef& acquired,
        const LoopPlanContext& current) const noexcept {
        if (!valid()) return LoopBindingStatus::MissingPlan;
        if (&directory != directory_) return LoopBindingStatus::WrongDirectory;
        if (!sameContext(context_, current)) return LoopBindingStatus::ContextMismatch;
        if (acquired.record != ref_.record || !recordMatches(acquired, current))
            return LoopBindingStatus::StalePublication;
        return LoopBindingStatus::Ok;
    }
    // END V1_VOICE_BINDING

    bool reset() noexcept {
        if (ref_.valid() && !directory_->release(ref_)) return false;
        directory_ = nullptr;
        context_ = {};
        fade_ = {};
        release_ = {};
        return true;
    }

private:
    friend class PinnedLoopPlayback;
    static bool validContext(const LoopPlanContext& c) noexcept {
        bool has_digest = false;
        for (const auto byte : c.asset_sha256) has_digest = has_digest || byte != 0;
        return has_digest && c.session_serial != 0 && c.bank_generation != 0 &&
            c.sample_generation != 0 && c.sample_frames != 0 && c.sample_rate != 0 &&
            c.policy_revision == 3 && c.coordinate_revision == 1 &&
            c.sample_type == 1 && c.noteon_addresses_fixed;
    }
    static bool recordMatches(const sample::PublishedVoiceRef& pin,
                              const LoopPlanContext& c) noexcept {
        if (!pin.valid() || pin.sample_id != c.sample_id ||
            pin.bank_generation != c.bank_generation || pin.sample_generation != c.sample_generation)
            return false;
        const auto& r = *pin.record;
        return r.pcm != nullptr && r.descriptor.id == c.sample_id &&
            r.bank_generation == c.bank_generation && r.sample_generation == c.sample_generation &&
            r.descriptor.frames == c.sample_frames && r.descriptor.sample_rate == c.sample_rate &&
            std::uint64_t{r.descriptor.pcm_bytes} >= std::uint64_t{c.sample_frames} * sizeof(std::int16_t);
    }
    sample::PublishedSampleDirectory* directory_ = nullptr;
    sample::PublishedVoiceRef ref_{};
    LoopPlanContext context_{};
    ExperimentalFade fade_{};
    ExperimentalReleaseBridge release_{};
};

// One independently owned PCM pin covers BOTH primary and release shadow.
// No pointer to the plan survives start: plan retirement cannot invalidate a
// playing voice. New starts must acquire the same CURRENTLY published record.
// Single-owner control/render calls; no additional locks or allocation here.
class PinnedLoopPlayback {
public:
    PinnedLoopPlayback() = default;
    ~PinnedLoopPlayback() { (void)reset(); }
    PinnedLoopPlayback(const PinnedLoopPlayback&) = delete;
    PinnedLoopPlayback& operator=(const PinnedLoopPlayback&) = delete;
    PinnedLoopPlayback(PinnedLoopPlayback&&) = delete;
    PinnedLoopPlayback& operator=(PinnedLoopPlayback&&) = delete;

    LoopBindingStatus start(sample::PublishedSampleDirectory& directory,
                            const PinnedLoopPlan& plan, const LoopPlanContext& current,
                            std::uint64_t step_q32 = LoopCrossfadeCursor::kOne) noexcept {
        if (ref_.valid()) return LoopBindingStatus::Busy;
        if (!plan.valid()) return LoopBindingStatus::MissingPlan;
        if (&directory != plan.directory_) return LoopBindingStatus::WrongDirectory;
        if (!sameContext(plan.context_, current)) return LoopBindingStatus::ContextMismatch;
        auto acquired = directory.acquire(current.sample_id);
        if (!acquired.valid()) return LoopBindingStatus::SampleUnavailable;
        // plan's pin prevents record-address reuse/ABA while this comparison runs.
        if (acquired.record != plan.ref_.record || !PinnedLoopPlan::recordMatches(acquired, current)) {
            (void)directory.release(acquired);
            return LoopBindingStatus::StalePublication;
        }
        BoundedLoopReleaseCursor candidate;
        if (!candidate.start(acquired.record->pcm, current.sample_frames, current.region,
                             current.mode, step_q32, plan.fade_, plan.release_)) {
            (void)directory.release(acquired);
            return LoopBindingStatus::InvalidConfiguration;
        }
        cursor_ = candidate; // local values only; no borrowed plan storage
        ref_ = std::move(acquired);
        directory_ = &directory;
        return LoopBindingStatus::Ok;
    }

    bool active() const noexcept { return cursor_.active(); }
    bool holdsSample() const noexcept { return ref_.valid(); }
    std::uint64_t phaseQ32() const noexcept { return cursor_.phaseQ32(); }
    bool transitionActive() const noexcept { return cursor_.transitionActive(); }
    void noteOff() noexcept { cursor_.noteOff(); }
    bool setStep(std::uint64_t step) noexcept { return cursor_.setStep(step); }

    bool next(std::int16_t& out) noexcept {
        if (!ref_.valid()) return false;
        const bool produced = cursor_.next(out);
        if (!cursor_.active()) (void)reset(); // clear BOTH borrowed readers before last unpin
        return produced;
    }
    std::size_t render(std::int16_t* out, std::size_t frames) noexcept {
        if (out == nullptr) return 0;
        std::size_t done = 0;
        while (done < frames && next(out[done])) ++done;
        return done;
    }
    bool reset() noexcept {
        cursor_.reset();
        if (ref_.valid() && !directory_->release(ref_)) return false;
        directory_ = nullptr;
        return true;
    }

private:
    sample::PublishedSampleDirectory* directory_ = nullptr;
    sample::PublishedVoiceRef ref_{};
    BoundedLoopReleaseCursor cursor_{};
};

} // namespace gm_synth::voice::experimental
