#pragma once

#include "gm/ProgramChangeControlService.h"
#include "gm/ZoneResolver.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "voice/experimental/PreparedLoopHandoff.h"

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP

namespace gm_synth::app {
namespace loop = voice::experimental;

// Runs ONLY on the serialized control task. A provider is an explicit experiment
// input, NOT an approval map or an on-device implementation of the Host scorer.
// Null means Direct. Context is derived independently from the current loader.
// Providers must not throw; they run in the bounded control-domain staging call.
using LoopProposalProvider = bool (*)(void*, const loop::PreparedLoopProgram&,
    std::uint16_t, const loop::LoopPlanContext&, loop::LoopPlanRequest&);

struct LoopStageReport {
    loop::PreparedLoopStatus status = loop::PreparedLoopStatus::NoReady;
    std::size_t examined = 0, admitted = 0, declined = 0;
    std::size_t context_rejected = 0, proposal_rejected = 0, capacity_skipped = 0;
};

// Caller-owned, fixed-capacity channel. Construct one ONLY for each configured
// channel rather than reserving MaxPlans * 16 plans unconditionally. Asset stamp
// is immutable for its lifetime; a bank/session change requires stopped audio,
// resetQuiescent(), and a newly constructed channel. The trusted loader must
// digest/pin the immutable source; comparing stamps does not authenticate files.
//
// Single control producer + single audio consumer. The router below must be the
// EXCLUSIVE commit and ack path for attached channels. External pair-only commits,
// cancellation, snapshot mutation, or direct service acknowledgement are forbidden.
// No reset/destruction races are allowed. Owners outlive plans AND sounding Voices.
template<std::size_t MaxPlans = 16>
class LoopProgramChannel {
public:
    enum class Phase : std::uint32_t { Idle, Preparing, Ready, Committed, Fault };
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    LoopProgramChannel(std::uint8_t channel, sample::SampleSource& source,
        sample::SampleResidencyManager& residency, sample::PublishedSampleDirectory& directory,
        sample::ProgramSnapshotExchange& samples, gm::PlaybackProgramSnapshotExchange& playback,
        loop::PreparedLoopAsset asset, LoopProposalProvider provider = nullptr, void* context = nullptr)
        : channel_(channel), source_(source), residency_(residency), directory_(directory),
          samples_(samples), playback_(playback), asset_(asset), provider_(provider),
          provider_context_(context), handoff_(directory), coordinator_(samples, playback) {}
    LoopProgramChannel(const LoopProgramChannel&) = delete;
    LoopProgramChannel& operator=(const LoopProgramChannel&) = delete;

    std::uint8_t channel() const noexcept { return channel_; }
    bool belongsTo(const sample::ProgramSnapshotExchange& s,
                   const gm::PlaybackProgramSnapshotExchange& p) const noexcept {
        return &s == &samples_ && &p == &playback_;
    }
    Phase phase() const noexcept { return static_cast<Phase>(phase_.load(std::memory_order_acquire)); }
    // Control owner only, or after joining the control task.
    const LoopStageReport& stageReport() const noexcept { return report_; }

    // Initial residency/playback are already Active. This publishes an initial
    // optional sidecar through the SAME gate; no early audio activation.
    loop::PreparedLoopStatus initializeActive() noexcept {
        if (!canPrepare()) return loop::PreparedLoopStatus::Busy;
        if (samples_.transitionInFlight(channel_) || playback_.transitionInFlight(channel_))
            return loop::PreparedLoopStatus::Busy;
        return prepareSidecar(playback_.active(channel_), samples_.active(channel_), true);
    }

    gm::ProgramChangeServiceResult service(gm::ProgramChangeControlService& control,
                                         gm::ProgramChangePrepareWorkspace& workspace) {
        if (!canPrepare()) {
            gm::ProgramChangeServiceResult result;
            result.status = gm::ProgramChangeServiceStatus::TransitionBusy;
            return result;
        }
        phase_.store(static_cast<std::uint32_t>(Phase::Preparing), std::memory_order_release);
        // Production service may publish its pair now; audio remains gated.
        auto result = control.serviceChannel(channel_, workspace);
        if (!result.ok()) {
            const bool pending = samples_.transitionInFlight(channel_) ||
                                 playback_.transitionInFlight(channel_);
            phase_.store(static_cast<std::uint32_t>(pending ? Phase::Fault : Phase::Idle),
                         std::memory_order_release);
            return result;
        }
        if (prepareSidecar(playback_.ready(channel_), samples_.ready(channel_), false) !=
                loop::PreparedLoopStatus::Ok)
            result.status = gm::ProgramChangeServiceStatus::PlaybackPublishInvariantFailed;
        return result;
    }

    // One audio thread, at block boundary BEFORE any event for that block.
    bool commitAtBlockBoundary() noexcept {
        if (phase() != Phase::Ready) return false;
        const bool matches = initial_
            ? next_.matches(playback_.active(channel_), samples_.active(channel_)) &&
              !samples_.transitionInFlight(channel_) && !playback_.transitionInFlight(channel_)
            : next_.matches(playback_.ready(channel_), samples_.ready(channel_)) &&
              coordinator_.readyCompatible(channel_);
        if (!matches || !handoff_.audioReadyMatches(next_.playback, next_.residency))
            return failAudio(); // no pair changed on this preflight failure
        if (!initial_ && !coordinator_.commitAtBlockBoundary(channel_)) return failAudio();
        if (handoff_.audioCommitAtBlockBoundary(playback_.active(channel_), samples_.active(channel_)) !=
                loop::PreparedLoopStatus::Ok) return failAudio();
        // Do NOT let the control task retire the pair after its token becomes
        // visible but before the sidecar switch finishes. This is the LAST store.
        phase_.store(static_cast<std::uint32_t>(Phase::Committed), std::memory_order_release);
        return true;
    }

    // Control owner only. Release old sidecar pins BEFORE retiring Program pins.
    // This publication is the only path returning retired workspace ownership.
    gm::ProgramChangeRetireAck acknowledge(gm::ProgramChangeControlService& control) {
        gm::ProgramChangeRetireAck ack;
        ack.status = gm::ProgramChangeServiceStatus::CommitNotReady;
        if (phase() != Phase::Committed) return ack;
        if (!next_.matches(playback_.active(channel_), samples_.active(channel_)) ||
            (!initial_ && (samples_.committedToken(channel_) != next_.token ||
                          playback_.committedToken(channel_) != next_.token))) {
            failAudio(); return ack;
        }
        if (handoff_.controlAcknowledge() != loop::PreparedLoopStatus::Ok) {
            failAudio(); return ack;
        }
        if (initial_) {
            ack.status = gm::ProgramChangeServiceStatus::Ok;
            ack.token = next_.token;
        } else {
            ack = control.acknowledgeCommitted(channel_);
            if (!ack.ok()) { failAudio(); return ack; }
        }
        next_ = {}; initial_ = false;
        phase_.store(static_cast<std::uint32_t>(Phase::Idle), std::memory_order_release);
        return ack;
    }

    // The existing resolver supplies exact zone index AND original MIDI event.
    // No source I/O, policy analysis, allocations or control work occurs here.
    bool start(const gm::PlaybackProgramSnapshot& program, std::uint16_t zone,
               const realtime::RealtimeEvent& event, const voice::VoiceStartParams& params,
               voice::VoicePool& pool) noexcept {
        if (phase() == Phase::Fault || event.channel != channel_ ||
            playback_.active(channel_) != &program || !coordinator_.activeCompatible(channel_))
            return false;
        bool started = false;
        const auto status = handoff_.startPoolVoice(zone, &program, samples_.active(channel_),
            event.data1, event.data2, pool, params, started);
        if (status == loop::LoopBindingStatus::Ok) return started;
        // Missing optional plan preserves ordinary production playback. A
        // mismatched active sidecar is an invariant fault, NOT a Direct retry.
        return status == loop::LoopBindingStatus::MissingPlan && pool.start(params);
    }

    // Stopped tasks only. Does NOT cancel or acknowledge external Program READY;
    // fault recovery must retain/rebuild the corresponding owners as a group.
    bool resetQuiescent() noexcept {
        if (!handoff_.resetQuiescent()) return false;
        next_ = {}; initial_ = false; report_ = {};
        phase_.store(static_cast<std::uint32_t>(Phase::Idle), std::memory_order_relaxed);
        return true;
    }
private:
    bool canPrepare() const noexcept {
        return channel_ < 16 && asset_.valid() && phase() == Phase::Idle;
    }
    bool failAudio() noexcept {
        phase_.store(static_cast<std::uint32_t>(Phase::Fault), std::memory_order_release);
        return false;
    }
    loop::PreparedLoopStatus prepareSidecar(const gm::PlaybackProgramSnapshot* p,
                                            const sample::ProgramSnapshot* r, bool initial) noexcept {
        phase_.store(static_cast<std::uint32_t>(Phase::Preparing), std::memory_order_release);
        report_ = {};
        loop::PreparedLoopContextBuilder builder;
        auto status = p && r ? builder.begin(source_, residency_, directory_, *p, *r, asset_)
                             : loop::PreparedLoopStatus::InvalidSnapshot;
        if (status == loop::PreparedLoopStatus::Ok) status = handoff_.controlBegin(builder);
        if (status != loop::PreparedLoopStatus::Ok) {
            report_.status = status; failAudio(); return status;
        }
        next_ = builder.program(); initial_ = initial;
        if (provider_) for (std::size_t i = 0; i < p->zone_count; ++i) {
            ++report_.examined;
            if (report_.admitted == MaxPlans) { ++report_.capacity_skipped; continue; }
            loop::LoopPlanContext current;
            if (builder.derive(static_cast<std::uint16_t>(i), current) != loop::PreparedLoopStatus::Ok) {
                ++report_.context_rejected; continue;
            }
            loop::LoopPlanRequest proposal;
            if (!provider_(provider_context_, next_, static_cast<std::uint16_t>(i), current, proposal)) {
                ++report_.declined; continue;
            }
            // Unsupported/stale/wrong identity proposals omit this optional
            // entry rather than aborting a valid Program Change.
            if (handoff_.controlAdd(builder, static_cast<std::uint16_t>(i), proposal) !=
                    loop::PreparedLoopStatus::Ok) { ++report_.proposal_rejected; continue; }
            ++report_.admitted;
        }
        status = handoff_.controlPublish();
        report_.status = status;
        phase_.store(static_cast<std::uint32_t>(status == loop::PreparedLoopStatus::Ok
                         ? Phase::Ready : Phase::Fault), std::memory_order_release);
        return status;
    }
    const std::uint8_t channel_;
    sample::SampleSource& source_;
    sample::SampleResidencyManager& residency_;
    sample::PublishedSampleDirectory& directory_;
    sample::ProgramSnapshotExchange& samples_;
    gm::PlaybackProgramSnapshotExchange& playback_;
    const loop::PreparedLoopAsset asset_;
    LoopProposalProvider provider_;
    void* provider_context_;
    loop::PreparedLoopHandoff<MaxPlans> handoff_;
    gm::ProgramRuntimeCommitCoordinator coordinator_;
    std::atomic<std::uint32_t> phase_{static_cast<std::uint32_t>(Phase::Idle)};
    loop::PreparedLoopProgram next_{};
    bool initial_ = false;
    LoopStageReport report_{};
};

// App-facing replacement for the pair-only block committer. All Program Change
// control, block commits, retire acks and NoteOn starts for attached channels go
// through this one object. Unattached channels retain the legacy pair protocol.
// Attach/detach and event hook installation are QUIESCENT initialization only.
template<std::size_t MaxPlans = 16>
class LoopProgramRuntime {
public:
    LoopProgramRuntime(gm::ProgramChangeControlService& control,
        sample::ProgramSnapshotExchange& samples, gm::PlaybackProgramSnapshotExchange& playback)
        : control_(control), samples_(samples), playback_(playback), coordinator_(samples, playback) {}
    bool attach(LoopProgramChannel<MaxPlans>& channel) noexcept {
        if (channel.channel() >= channels_.size() || channels_[channel.channel()] ||
            !channel.belongsTo(samples_, playback_)) return false;
        channels_[channel.channel()] = &channel;
        return true;
    }
    void install(gm::RealtimeProgramEventApplier& events) noexcept {
        events.setExperimentalVoiceStarter(&startCallback, this);
    }
    void uninstallQuiescent(gm::RealtimeProgramEventApplier& events) noexcept {
        events.setExperimentalVoiceStarter(nullptr, nullptr);
    }
    std::size_t drainRealtimeRequests() { return control_.drainRealtimeRequests(); }
    gm::ProgramChangeServiceResult serviceChannel(std::uint8_t channel,
                                                 gm::ProgramChangePrepareWorkspace& workspace) {
        if (channel < channels_.size() && channels_[channel])
            return channels_[channel]->service(control_, workspace);
        return control_.serviceChannel(channel, workspace);
    }
    gm::ProgramChangeRetireAck acknowledgeCommitted(std::uint8_t channel) {
        if (channel < channels_.size() && channels_[channel])
            return channels_[channel]->acknowledge(control_);
        return control_.acknowledgeCommitted(channel);
    }
    std::size_t commitReadyAtBlockBoundary() noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            const auto channel = static_cast<std::uint8_t>(i);
            if (channels_[i] ? channels_[i]->commitAtBlockBoundary()
                             : coordinator_.commitAtBlockBoundary(channel)) ++count;
        }
        return count;
    }
    void render(realtime::ScheduledAudioBlockRenderer& renderer, std::uint64_t frame,
                std::int16_t* stereo) {
        (void)commitReadyAtBlockBoundary();
        renderer.render(frame, stereo);
    }
private:
    static bool startCallback(void* opaque, const gm::PlaybackProgramSnapshot& program,
        std::uint16_t zone, const realtime::RealtimeEvent& event,
        const voice::VoiceStartParams& params, voice::VoicePool& voices) {
        auto& self = *static_cast<LoopProgramRuntime*>(opaque);
        if (event.channel >= self.channels_.size()) return false;
        auto* channel = self.channels_[event.channel];
        return channel ? channel->start(program, zone, event, params, voices) : voices.start(params);
    }
    gm::ProgramChangeControlService& control_;
    sample::ProgramSnapshotExchange& samples_;
    gm::PlaybackProgramSnapshotExchange& playback_;
    gm::ProgramRuntimeCommitCoordinator coordinator_;
    std::array<LoopProgramChannel<MaxPlans>*, 16> channels_{};
};
} // namespace gm_synth::app
#endif // GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
