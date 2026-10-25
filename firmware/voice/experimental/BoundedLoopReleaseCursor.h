#pragma once

#include "LoopCrossfadeCursor.h"

namespace gm_synth::voice::experimental {

// Separate opt-in experiment; production Voice and the frozen transport remain
// unchanged. Duration is OUTPUT frames, unlike ExperimentalFade's source frames.
struct ExperimentalReleaseBridge {
    bool enabled = false;
    std::uint16_t output_frames = 0;
};

class BoundedLoopReleaseCursor {
public:
    bool start(const std::int16_t* pcm, std::uint32_t frames, CursorRegion region,
               CursorLoopMode mode, std::uint64_t step_q32 = LoopCrossfadeCursor::kOne,
               ExperimentalFade fade = {}, ExperimentalReleaseBridge release = {}) noexcept {
        if (active() || (release.enabled && release.output_frames != 16 &&
                        release.output_frames != 32 && release.output_frames != 64)) return false;
        LoopCrossfadeCursor candidate;
        if (!candidate.start(pcm, frames, region, mode, step_q32, fade)) return false;
        reset();
        primary_ = candidate;
        duration_ = release.enabled ? release.output_frames : 0;
        return true;
    }

    bool active() const noexcept { return primary_.active(); }
    bool looping() const noexcept { return primary_.looping(); }
    std::uint64_t phaseQ32() const noexcept { return primary_.phaseQ32(); }
    bool transitionActive() const noexcept { return bridging_; }
    bool transitionStarted() const noexcept { return started_; }
    bool transitionTruncated() const noexcept { return truncated_; }
    std::uint16_t transitionFramesRendered() const noexcept { return rendered_; }

    bool setStep(std::uint64_t step_q32) noexcept {
        if (!primary_.setStep(step_q32)) return false;
        if (bridging_) (void)shadow_.setStep(step_q32);
        return true;
    }

    // Primary transport exits immediately at the same source phase: no rewind,
    // no delayed NoteOff and no lifetime extension. A borrowed shadow may keep
    // looping for at most duration_ OUTPUT samples, solely as a mixing source.
    // Repeated NoteOff is idempotent; sustain/envelope decisions belong upstream.
    void noteOff() noexcept {
        if (!active() || released_) return;
        const auto before = primary_;
        primary_.noteOff();
        released_ = true;
        if (!duration_ || !before.fadeFrames() || !before.looping() || primary_.looping()) return;
        auto old_probe = before;
        auto raw_probe = primary_;
        std::int16_t old_value = 0, raw_value = 0;
        if (!old_probe.next(old_value) || !raw_probe.next(raw_value) || old_value == raw_value) return;
        shadow_ = before;
        bridging_ = started_ = true;
    }

    bool next(std::int16_t& out) noexcept {
        std::int16_t raw = 0;
        if (!primary_.next(raw)) return false;
        out = raw;
        if (!bridging_) return true;
        std::int16_t held = 0;
        // A valid held-loop shadow cannot finish. Fail closed to the raw tail
        // if that invariant is ever broken; never read beyond a PCM region.
        if (!shadow_.next(held)) {
            truncated_ = true;
            bridging_ = false;
            shadow_.reset();
            return true;
        }
        const std::uint16_t denominator = duration_ - 1u;
        const bool last_raw_frame = !primary_.active();
        const auto weight = last_raw_frame ? denominator : rendered_;
        const std::int64_t numerator = std::int64_t{held} * (denominator - weight) +
                                       std::int64_t{raw} * weight;
        // Convex blend of PCM16 values: no clipping or corrective DC offset.
        // Denominator is odd, so exact half-way rounding ties cannot occur.
        out = static_cast<std::int16_t>(numerator >= 0
            ? (numerator + denominator / 2u) / denominator
            : -((-numerator + denominator / 2u) / denominator));
        if (last_raw_frame && rendered_ < denominator) truncated_ = true;
        ++rendered_;
        if (rendered_ == duration_ || last_raw_frame) {
            bridging_ = false;
            shadow_.reset();
        }
        return true;
    }

    std::size_t render(std::int16_t* out, std::size_t frames) noexcept {
        if (out == nullptr) return 0;
        std::size_t done = 0;
        while (done < frames && next(out[done])) ++done;
        return done;
    }
    void reset() noexcept { *this = BoundedLoopReleaseCursor{}; }

private:
    // Both cursors borrow the SAME immutable PCM. Their caller must pin it
    // throughout primary lifetime; this class owns no SampleRef or allocation.
    LoopCrossfadeCursor primary_{}, shadow_{};
    std::uint16_t duration_ = 0, rendered_ = 0;
    bool released_ = false, bridging_ = false, started_ = false, truncated_ = false;
};

} // namespace gm_synth::voice::experimental
