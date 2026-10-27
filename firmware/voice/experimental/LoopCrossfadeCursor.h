#pragma once

#include <cstddef>
#include <cstdint>

namespace gm_synth::voice::experimental {

// Host-qualified transport prototype, NOT wired into Voice/Program Prepare.
// Caller owns immutable mono PCM for the entire active lifetime. No allocations,
// file access, locks or logging. All coordinates are source frames, not output
// frames. Control and render calls must be serialized by the caller.
enum class CursorLoopMode : std::uint8_t { None = 0, Continuous = 1, UntilRelease = 3 };
struct CursorRegion {
    std::uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0;
};
struct ExperimentalFade {
    bool enabled = false;
    std::uint16_t frames = 0;
};

class LoopCrossfadeCursor {
public:
    static constexpr std::uint64_t kOne = std::uint64_t{1} << 32;

    // Transactional: invalid arguments or an active cursor leave state intact.
    // An explicit enabled=true AND 16/32/64 frames is required for crossfade.
    bool start(const std::int16_t* pcm, std::uint32_t frames, CursorRegion region,
               CursorLoopMode mode, std::uint64_t step_q32 = kOne,
               ExperimentalFade fade = {}) noexcept {
        if (active_ || pcm == nullptr || frames == 0 || step_q32 == 0 ||
            region.start >= region.end || region.end > frames ||
            (mode != CursorLoopMode::None && mode != CursorLoopMode::Continuous &&
             mode != CursorLoopMode::UntilRelease)) return false;
        if (mode != CursorLoopMode::None &&
            !(region.start <= region.loop_start && region.loop_start < region.loop_end &&
              region.loop_end <= region.end)) return false;
        if (fade.enabled &&
            (mode == CursorLoopMode::None ||
             (fade.frames != 16 && fade.frames != 32 && fade.frames != 64) ||
             fade.frames >= (region.loop_end - region.loop_start) / 2u)) return false;
        pcm_ = pcm;
        region_ = region;
        mode_ = mode;
        fade_ = fade.enabled ? fade.frames : 0;
        step_ = step_q32;
        phase_ = std::uint64_t{region.start} << 32;
        released_ = false;
        active_ = true;
        return true;
    }

    bool active() const noexcept { return active_; }
    bool looping() const noexcept {
        return active_ && (mode_ == CursorLoopMode::Continuous ||
               (mode_ == CursorLoopMode::UntilRelease && !released_));
    }
    std::uint64_t phaseQ32() const noexcept { return phase_; }
    std::uint16_t fadeFrames() const noexcept { return fade_; }
    bool setStep(std::uint64_t step_q32) noexcept {
        if (!active_ || step_q32 == 0) return false;
        step_ = step_q32;
        return true;
    }
    // Matches UntilRelease transport semantics: no rewind and no further wrap.
    // Mid-blend release immediately returns to raw tail PCM. This transition is
    // NOT claimed click-free. Envelope/declick/sustain routing belongs upstream.
    void noteOff() noexcept { if (active_) released_ = true; }
    void reset() noexcept { *this = LoopCrossfadeCursor{}; }

    bool next(std::int16_t& out) noexcept {
        if (!active_) return false;
        const auto i = static_cast<std::uint32_t>(phase_ >> 32);
        std::uint32_t j = i + 1u;
        const bool loop = looping();
        if (loop && i >= region_.loop_start && j >= region_.loop_end)
            j = region_.loop_start + fade_;
        else if (j >= region_.end) j = region_.end - 1u;
        const std::int32_t a = value(i, loop), b = value(j, loop);
        const std::int64_t scaled = (std::int64_t{b} - a) *
            static_cast<std::uint32_t>(phase_);
        // Explicit floor matches the existing signed >>32 interpolation on the
        // supported compilers, without relying on negative-shift semantics.
        const std::int64_t delta = scaled >= 0 ? scaled / static_cast<std::int64_t>(kOne)
            : -((-scaled + static_cast<std::int64_t>(kOne - 1)) /
                 static_cast<std::int64_t>(kOne));
        out = static_cast<std::int16_t>(a + delta);
        advance(loop);
        return true;
    }

    // Writes exactly the returned count; null/zero-length calls do not advance.
    std::size_t render(std::int16_t* out, std::size_t count) noexcept {
        if (out == nullptr) return 0;
        std::size_t done = 0;
        while (done < count && next(out[done])) ++done;
        return done;
    }

private:
    std::int32_t value(std::uint32_t i, bool loop) const noexcept {
        if (!loop || fade_ == 0 || i < region_.loop_end - fade_ ||
            i >= region_.loop_end) return pcm_[i];
        const std::uint32_t k = i - (region_.loop_end - fade_);
        const std::int64_t numerator = std::int64_t{pcm_[i]} * (fade_ - k) +
            std::int64_t{pcm_[region_.loop_start + k]} * (k + 1u);
        const std::int32_t denominator = fade_ + 1;
        // Allowed N are even: denominator is odd, so no half-way tie exists.
        // This is exactly Python round(numerator/(N+1)) on PCM16 inputs.
        return static_cast<std::int32_t>(numerator >= 0
            ? (numerator + denominator / 2) / denominator
            : -((-numerator + denominator / 2) / denominator));
    }

    void advance(bool loop) noexcept {
        const std::uint64_t end = std::uint64_t{loop ? region_.loop_end : region_.end} << 32;
        const std::uint64_t distance = end - phase_;
        if (step_ < distance) { phase_ += step_; return; }
        if (!loop) { active_ = false; phase_ = end; return; }
        const std::uint64_t resume = std::uint64_t{region_.loop_start + fade_} << 32;
        // Subtract before adding to prevent uint64 wrap at extreme pitch steps.
        // One modulo handles arbitrary overshoot; no loop proportional to pitch.
        phase_ = resume + (step_ - distance) % (end - resume);
    }

    const std::int16_t* pcm_ = nullptr;
    CursorRegion region_{};
    CursorLoopMode mode_ = CursorLoopMode::None;
    std::uint64_t phase_ = 0, step_ = kOne;
    std::uint16_t fade_ = 0;
    bool active_ = false, released_ = false;
};

} // namespace gm_synth::voice::experimental
