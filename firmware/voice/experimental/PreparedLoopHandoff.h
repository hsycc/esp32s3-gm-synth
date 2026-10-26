#pragma once

#include "PinnedLoopPlan.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "sample/SampleSource.h"
#include <atomic>
#include <limits>

namespace gm_synth::voice::experimental {

// Control-domain loader stamp, independently of any fade proposal. The loader
// must hash/pin the immutable source and allocate a non-reused session serial.
// This adapter does not authenticate a supplied digest or allow mutable files.
struct PreparedLoopAsset {
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t session = 0;
    std::uint32_t bank_generation = 0;
    bool valid() const noexcept {
        bool nonzero = false;
        for (auto b : sha256) nonzero = nonzero || b != 0;
        return nonzero && session != 0 && bank_generation != 0;
    }
};

struct PreparedLoopProgram {
    const gm::PlaybackProgramSnapshot* playback = nullptr;
    const sample::ProgramSnapshot* residency = nullptr;
    std::uint8_t channel = 0, program = 0;
    std::uint16_t bank = 0;
    std::uint32_t token = 0, bank_generation = 0;
    PreparedLoopAsset asset{};

    // Audio-side constant-time check. Pointers must be the CURRENT immutable
    // snapshots supplied by the existing exchanges, not caller-created copies.
    bool matches(const gm::PlaybackProgramSnapshot* p,
                 const sample::ProgramSnapshot* r) const noexcept {
        return p && r && p == playback && r == residency &&
            p->channel == channel && r->channel == channel &&
            p->bank == bank && r->bank == bank &&
            p->program == program && r->program == program &&
            p->token == token && r->token == token &&
            p->bank_generation == bank_generation && r->bank_generation == bank_generation;
    }
};

enum class PreparedLoopStatus : std::uint8_t {
    Ok, InvalidAsset, InvalidSnapshot, ProgramMismatch, InvalidZone,
    MissingDependency, DynamicAddress, UnsupportedSample, NotPinned,
    SourceMismatch, PublicationMismatch, Busy, Full, DuplicateZone,
    ProposalRejected, NoReady, CommitRejected, NoActivePlan, NoAcknowledgement,
    ReleaseFault
};

// Control-only bridge using REAL prepared zones, source descriptors, program
// dependency pins and directory records. begin/derive may perform source I/O;
// neither is an audio API. All source/residency/Prepare mutations must be
// serialized on the control task while this builder is used. Snapshot storage
// must stay immutable until the handoff acknowledgement, as in production.
class PreparedLoopContextBuilder {
public:
    PreparedLoopStatus begin(sample::SampleSource& source,
                             sample::SampleResidencyManager& residency,
                             sample::PublishedSampleDirectory& directory,
                             const gm::PlaybackProgramSnapshot& playback,
                             const sample::ProgramSnapshot& resident,
                             PreparedLoopAsset asset) noexcept {
        // A failed rebind must not leave an old builder usable accidentally.
        source_ = nullptr;
        if (!asset.valid()) return PreparedLoopStatus::InvalidAsset;
        if (!playback.valid() || !resident.valid()) return PreparedLoopStatus::InvalidSnapshot;
        if (playback.channel != resident.channel || playback.bank != resident.bank ||
            playback.program != resident.program || playback.token != resident.token ||
            playback.bank_generation != resident.bank_generation ||
            resident.bank_generation != directory.bankGeneration() ||
            asset.bank_generation != resident.bank_generation)
            return PreparedLoopStatus::ProgramMismatch;
        source_ = &source; residency_ = &residency; directory_ = &directory;
        program_ = {&playback, &resident, playback.channel, playback.program,
                    playback.bank, playback.token, playback.bank_generation, asset};
        return PreparedLoopStatus::Ok;
    }
    bool valid() const noexcept { return source_ != nullptr; }
    PreparedLoopProgram program() const noexcept { return program_; }

    PreparedLoopStatus derive(std::uint16_t zone_index, LoopPlanContext& out) noexcept {
        if (!valid() || !program_.matches(program_.playback, program_.residency))
            return PreparedLoopStatus::InvalidSnapshot;
        const auto& p = *program_.playback;
        const auto& r = *program_.residency;
        if (zone_index >= p.zone_count || !p.zones) return PreparedLoopStatus::InvalidZone;
        const auto& z = p.zones[zone_index];
        if (!z.valid()) return PreparedLoopStatus::InvalidZone;
        bool dependency = false;
        for (std::size_t i = 0; i < r.dependency_count; ++i)
            dependency = dependency || r.sample_ids[i] == z.sample_id;
        if (!dependency) return PreparedLoopStatus::MissingDependency;
        // Reject every address destination, including future coarse/loop routes.
        for (std::size_t i = 0; i < z.prepared_note_on_modulators.count; ++i) {
            const auto d = z.prepared_note_on_modulators.routes[i].destination_oper;
            if (d == 0 || d == 1 || d == 2 || d == 3 || d == 4 || d == 12 || d == 45 || d == 50)
                return PreparedLoopStatus::DynamicAddress;
        }
        sample::SampleDescriptor expected{};
        if (!source_->descriptorFor(z.sample_id, &expected)) return PreparedLoopStatus::SourceMismatch;
        if (expected.flags != 1) return PreparedLoopStatus::UnsupportedSample;
        const auto view = residency_->residentView(z.sample_id);
        if (!view.valid() || view.pin_count == 0) return PreparedLoopStatus::NotPinned;
        if (expected != view.descriptor || expected.id != z.sample_id || !expected.frames ||
            !expected.sample_rate || std::uint64_t{expected.pcm_bytes} != std::uint64_t{expected.frames} * 2u)
            return PreparedLoopStatus::SourceMismatch;
        auto pin = directory_->acquire(z.sample_id);
        if (!pin.valid()) return PreparedLoopStatus::PublicationMismatch;
        const auto& record = *pin.record;
        const bool same = record.pcm == view.pcm && record.descriptor == expected &&
            pin.bank_generation == program_.bank_generation &&
            pin.sample_generation == view.generation && record.sample_generation == view.generation;
        LoopPlanContext candidate{};
        candidate.asset_sha256 = program_.asset.sha256;
        candidate.session_serial = program_.asset.session;
        candidate.bank_generation = pin.bank_generation;
        candidate.sample_generation = pin.sample_generation;
        candidate.sample_id = z.sample_id;
        candidate.sample_frames = expected.frames;
        candidate.sample_rate = expected.sample_rate;
        candidate.sample_type = expected.flags;
        candidate.noteon_addresses_fixed = true;
        const auto& sr = z.sample_region;
        candidate.region = {sr.enabled ? sr.start_frame : 0u,
                            sr.enabled ? sr.end_frame : expected.frames,
                            sr.enabled ? sr.loop_start_frame : expected.loop_start,
                            sr.enabled ? sr.loop_end_frame : expected.loop_end};
        const bool mode_ok = z.loop_mode == voice::LoopMode::None ||
            z.loop_mode == voice::LoopMode::Continuous || z.loop_mode == voice::LoopMode::UntilRelease;
        candidate.mode = z.loop_mode == voice::LoopMode::None ? CursorLoopMode::None :
            z.loop_mode == voice::LoopMode::Continuous ? CursorLoopMode::Continuous : CursorLoopMode::UntilRelease;
        if (candidate.mode == CursorLoopMode::None) candidate.region.loop_start = candidate.region.loop_end = 0;
        LoopCrossfadeCursor geometry_check;
        const bool geometry_ok = same && mode_ok && geometry_check.start(record.pcm, expected.frames,
                                                                       candidate.region, candidate.mode);
        geometry_check.reset();
        if (!directory_->release(pin)) return PreparedLoopStatus::ReleaseFault;
        if (!same) return PreparedLoopStatus::PublicationMismatch;
        if (!geometry_ok) return PreparedLoopStatus::InvalidZone;
        out = candidate; // output untouched on every failure
        return PreparedLoopStatus::Ok;
    }
private:
    sample::SampleSource* source_ = nullptr;
    sample::SampleResidencyManager* residency_ = nullptr;
    sample::PublishedSampleDirectory* directory_ = nullptr;
    PreparedLoopProgram program_{};
};

// Bounded SPSC sidecar for ONE channel, with two fixed buffers and up to N
// selected zones per program. Not wired into the application. Control alone
// calls begin/add/publish/cancel/acknowledge. One audio task alone calls commit
// at block boundaries and start between boundaries. Never call reset/destruct
// until both tasks are stopped. Directory and source owners outlive this object.
//
// Idle -> Ready release/acquire publishes fully bound immutable plans.
// Ready -> Committed release/acquire acknowledges that audio can no longer
// access the retired buffer. ONLY control then resets its pins. A playback
// owns a separate ref and no plan pointer, so old tails survive this reset.
//
// Existing residency AND playback exchanges must commit first at the SAME
// boundary. A mismatch rejects the sidecar, never partially activates it.
// This class cannot make two external exchanges atomic; the future app must
// enforce that order and keep all snapshots alive until both acknowledgements.
template<std::size_t MaxPlans = 16>
class PreparedLoopHandoff {
    static_assert(MaxPlans > 0 && MaxPlans <= gm::PlaybackProgramSnapshot::kMaxZones);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "handoff state requires lock-free 32-bit atomics");
public:
    explicit PreparedLoopHandoff(sample::PublishedSampleDirectory& directory) : directory_(directory) {}
    PreparedLoopHandoff(const PreparedLoopHandoff&) = delete;
    PreparedLoopHandoff& operator=(const PreparedLoopHandoff&) = delete;
    PreparedLoopHandoff(PreparedLoopHandoff&&) = delete;
    PreparedLoopHandoff& operator=(PreparedLoopHandoff&&) = delete;

    PreparedLoopStatus controlBegin(const PreparedLoopContextBuilder& builder) noexcept {
        if (!builder.valid()) return PreparedLoopStatus::InvalidSnapshot;
        if (building_ != absent || phase_.load(std::memory_order_acquire) != Idle)
            return PreparedLoopStatus::Busy;
        for (unsigned i = 0; i < 2; ++i) if (free_[i]) {
            free_[i] = false; building_ = i;
            buffers_[i].program = builder.program();
            return PreparedLoopStatus::Ok;
        }
        return PreparedLoopStatus::Full;
    }
    PreparedLoopStatus controlAdd(PreparedLoopContextBuilder& builder, std::uint16_t zone_index,
                                  const LoopPlanRequest& proposal) noexcept {
        if (building_ == absent) return PreparedLoopStatus::Busy;
        auto& b = buffers_[building_];
        const auto current = builder.program();
        if (!builder.valid() || !b.program.matches(current.playback, current.residency) ||
            b.program.asset.sha256 != current.asset.sha256 || b.program.asset.session != current.asset.session)
            return PreparedLoopStatus::ProgramMismatch;
        for (std::size_t i = 0; i < b.count; ++i)
            if (b.entries[i].zone == zone_index) return PreparedLoopStatus::DuplicateZone;
        if (b.count == MaxPlans) return PreparedLoopStatus::Full;
        LoopPlanContext context{};
        const auto result = builder.derive(zone_index, context);
        if (result != PreparedLoopStatus::Ok) return result;
        auto& e = b.entries[b.count];
        if (e.plan.bind(directory_, proposal, context) != LoopBindingStatus::Ok)
            return PreparedLoopStatus::ProposalRejected;
        e.zone = zone_index;
        ++b.count;
        return PreparedLoopStatus::Ok;
    }
    PreparedLoopStatus controlPublish() noexcept {
        if (building_ == absent || phase_.load(std::memory_order_acquire) != Idle)
            return PreparedLoopStatus::Busy;
        ready_ = building_; building_ = absent;
        phase_.store(Ready, std::memory_order_release);
        return PreparedLoopStatus::Ok;
    }
    PreparedLoopStatus controlCancel() noexcept {
        if (building_ == absent) return PreparedLoopStatus::Busy;
        if (!clear(building_)) return PreparedLoopStatus::ReleaseFault;
        building_ = absent;
        return PreparedLoopStatus::Ok;
    }
    PreparedLoopStatus audioCommitAtBlockBoundary(const gm::PlaybackProgramSnapshot* playback,
                                                  const sample::ProgramSnapshot* residency) noexcept {
        if (phase_.load(std::memory_order_acquire) != Ready) return PreparedLoopStatus::NoReady;
        const bool accepted = buffers_[ready_].program.matches(playback, residency);
        retired_ = accepted ? active_ : ready_;
        if (accepted) active_ = ready_;
        phase_.store(Committed, std::memory_order_release);
        return accepted ? PreparedLoopStatus::Ok : PreparedLoopStatus::CommitRejected;
    }
    PreparedLoopStatus controlAcknowledge() noexcept {
        if (phase_.load(std::memory_order_acquire) != Committed)
            return PreparedLoopStatus::NoAcknowledgement;
        if (retired_ != absent && !clear(retired_)) return PreparedLoopStatus::ReleaseFault;
        retired_ = ready_ = absent;
        phase_.store(Idle, std::memory_order_release);
        return PreparedLoopStatus::Ok;
    }
    LoopBindingStatus audioStart(std::uint16_t zone, const gm::PlaybackProgramSnapshot* playback,
                                 const sample::ProgramSnapshot* residency,
                                 PinnedLoopPlayback& voice,
                                 std::uint64_t step = LoopCrossfadeCursor::kOne) noexcept {
        if (active_ == absent) return LoopBindingStatus::MissingPlan;
        const auto& b = buffers_[active_];
        if (!b.program.matches(playback, residency)) return LoopBindingStatus::ContextMismatch;
        for (std::size_t i = 0; i < b.count; ++i) if (b.entries[i].zone == zone)
            return voice.start(directory_, b.entries[i].plan, b.entries[i].plan.context(), step);
        return LoopBindingStatus::MissingPlan; // caller retains Direct; no automatic policy decision
    }
    // BEGIN V1_VOICE_BINDING
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // One audio task only, between block boundaries. Uses the same immutable
    // buffer/ack contract as start(); no plan pointer escapes synchronous start.
    LoopBindingStatus startVoice(std::uint16_t zone,
                                 const gm::PlaybackProgramSnapshot* playback,
                                 const sample::ProgramSnapshot* residency,
                                 voice::Voice& target,
                                 const voice::VoiceStartParams& params) noexcept {
        if (active_ == absent) return LoopBindingStatus::MissingPlan;
        const auto& b = buffers_[active_];
        if (!b.program.matches(playback, residency) || zone >= playback->zone_count ||
            params.channel != playback->channel || !playback->zones[zone].matches(params.key, params.velocity))
            return LoopBindingStatus::ContextMismatch;
        if (target.active()) return LoopBindingStatus::Busy;
        for (std::size_t i = 0; i < b.count; ++i) if (b.entries[i].zone == zone) {
            const auto context = b.entries[i].plan.context();
            auto requested = params;
            requested.experimental_loop_opt_in = true;
            requested.experimental_loop_plan = &b.entries[i].plan;
            requested.experimental_loop_context = &context;
            return target.start(directory_, requested) ? LoopBindingStatus::Ok :
                LoopBindingStatus::InvalidConfiguration;
        }
        return LoopBindingStatus::MissingPlan;
    }
#endif
    // END V1_VOICE_BINDING
    // BEGIN V1_APP_LOOP
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // Audio-owner preflight before ANY of the three commits. The application
    // gate keeps control from replacing READY until the joint acknowledgement.
    bool audioReadyMatches(const gm::PlaybackProgramSnapshot* playback,
                           const sample::ProgramSnapshot* residency) const noexcept {
        return phase_.load(std::memory_order_acquire) == Ready &&
            buffers_[ready_].program.matches(playback, residency);
    }

    // This method calls VoicePool exactly once for a selected plan. Admission
    // failure must not be retried as Direct (which would alter stealing stats).
    // `raw_key/velocity` are the incoming selection values; params.velocity may
    // already be overridden by SF2 fixedVelocity and is NOT the zone selector.
    LoopBindingStatus startPoolVoice(std::uint16_t zone,
                                    const gm::PlaybackProgramSnapshot* playback,
                                    const sample::ProgramSnapshot* residency,
                                    std::uint8_t raw_key, std::uint8_t raw_velocity,
                                    voice::VoicePool& pool,
                                    const voice::VoiceStartParams& params,
                                    bool& started) noexcept {
        started = false;
        if (active_ == absent) return LoopBindingStatus::MissingPlan;
        const auto& b = buffers_[active_];
        if (!b.program.matches(playback, residency) || zone >= playback->zone_count ||
            params.channel != playback->channel || params.key != raw_key ||
            !playback->zones[zone].matches(raw_key, raw_velocity) ||
            params.sample_id != playback->zones[zone].sample_id)
            return LoopBindingStatus::ContextMismatch;
        for (std::size_t i = 0; i < b.count; ++i) {
            if (b.entries[i].zone != zone) continue;
            const auto context = b.entries[i].plan.context();
            auto requested = params;
            requested.experimental_loop_opt_in = true;
            requested.experimental_loop_plan = &b.entries[i].plan;
            requested.experimental_loop_context = &context;
            started = pool.start(requested);
            return LoopBindingStatus::Ok;
        }
        return LoopBindingStatus::MissingPlan;
    }
#endif
    // END V1_APP_LOOP
    // Quiescent only. Not a racing cancel or a realtime shutdown API.
    bool resetQuiescent() noexcept {
        if (!clear(0) || !clear(1)) return false;
        active_ = ready_ = retired_ = building_ = absent;
        phase_.store(Idle, std::memory_order_relaxed);
        return true;
    }
private:
    static constexpr unsigned absent = 2;
    enum : std::uint32_t { Idle, Ready, Committed };
    struct Entry { PinnedLoopPlan plan{}; std::uint16_t zone = 0; };
    struct Buffer {
        PreparedLoopProgram program{};
        std::array<Entry, MaxPlans> entries{};
        std::size_t count = 0;
    };
    bool clear(unsigned index) noexcept { // control only, after acknowledgement
        auto& b = buffers_[index];
        for (std::size_t i = 0; i < b.count; ++i) if (!b.entries[i].plan.reset()) return false;
        b.count = 0; b.program = {}; free_[index] = true;
        return true;
    }
    sample::PublishedSampleDirectory& directory_;
    std::array<Buffer, 2> buffers_{};
    std::atomic<std::uint32_t> phase_{Idle};
    unsigned building_ = absent, ready_ = absent, retired_ = absent, active_ = absent;
    bool free_[2]{true, true};
};

} // namespace gm_synth::voice::experimental
