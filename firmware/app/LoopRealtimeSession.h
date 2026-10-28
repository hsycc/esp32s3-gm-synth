#pragma once

#include "app/LoopStartupSession.h"
#include <algorithm>
#include <atomic>
#include <type_traits>

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
namespace gm_synth::app {

struct LoopRealtimeConfig : LoopStartupConfig {
    // Control/startup only. Called after at most 32 KiB or 64 reader calls.
    // May yield/cancel, but must not re-enter this session or mutate its source.
    bool (*io_checkpoint)(void*) = nullptr;
    void* io_context = nullptr;
    // Optional control-only diagnostic clock. No clock calls enter render.
    std::uint64_t (*now_us)() = nullptr;
};

struct LoopRealtimeIoStats {
    std::uint64_t reads = 0, bytes = 0, checkpoints = 0, cancelled = 0;
    std::uint64_t read_total_us = 0, read_max_us = 0;
};
struct LoopRealtimeAnalysisTiming {
    std::uint64_t calls = 0, total_us = 0, max_us = 0;
};

template<std::size_t MaxZones, std::size_t MaxPlans> class LoopRealtimeSession;

// These are the ONLY externally placeable objects. No atomics, directory,
// published ResidentRecords, handoffs, Voice, event queue or FX state belong here.
// The allocator/hash/pin manager is control-only; final audio ownership uses
// INTERNAL PublishedSampleDirectory/ResidentRecord, never its phase-A leases.
// Scratch is control-owned; generation arrays become immutable from READY to
// retirement ACK. PSRAM placement does not permit reuse before that ACK.
template<std::size_t MaxZones>
struct LoopRealtimeStorage {
    static_assert(MaxZones > 0 && MaxZones <= 512);
    static_assert(std::is_trivially_copyable<gm::PlaybackZone>::value);
    static_assert(std::is_trivially_copyable<gm::PlaybackProgramSnapshot>::value);
    std::array<sf2::NativeResolvedZone, MaxZones> native{};
    std::array<sf2::ResolvedZoneModulation, MaxZones> mods{};
    struct Generation {
        std::array<gm::PlaybackZone, MaxZones> zones{};
        std::array<gm::PlaybackProgramKeySpan, 128> spans{};
        std::array<std::uint16_t, MaxZones * 128> indices{};
        std::array<sample::SampleId, MaxZones> deps{};
        gm::ProgramChangePrepareWorkspace w{};
    } a, b;
    LoopRealtimeStorage() {
        for (auto* generation : {&a, &b}) {
            auto& w = generation->w;
            w.native_scratch = native.data(); w.native_capacity = native.size();
            w.modulation_scratch = mods.data(); w.modulation_capacity = mods.size();
            w.playback_zones = generation->zones.data(); w.playback_capacity = generation->zones.size();
            w.key_spans = generation->spans.data(); w.key_zone_indices = generation->indices.data();
            w.key_zone_index_capacity = generation->indices.size();
            w.dependencies = generation->deps.data(); w.dependency_capacity = generation->deps.size();
        }
    }
    LoopRealtimeStorage(const LoopRealtimeStorage&) = delete;
    LoopRealtimeStorage& operator=(const LoopRealtimeStorage&) = delete;
private:
    template<std::size_t, std::size_t> friend class LoopRealtimeSession;
    // Session controls this lifetime too: destroy after the internal Graph,
    // before releasing the PCM lease. Caller cannot reuse an active allocator.
    std::optional<sample::SampleResidencyManager> residency_;
};

// One control actor + one audio actor + ONE event producer (normally control).
// This is a new ownership boundary; LoopStartupSession remains serial-only.
//
// The whole Session MUST remain INTERNAL on ESP32-S3. Storage may be in PSRAM
// under the exact field/immutability contract above. The caller owns Storage
// and must retain it until both actors are joined and stopQuiescent succeeds.
// start/beginActors are quiescent. After beginActors, audio APIs belong only to
// audio and control APIs only to control; report getters obey the same rule.
// Neither actor destroys resources. Stop publishes a request; each owner drops
// its work and publishes its own final ACK. The coordinator joins BOTH tasks
// and quiesces the event producer (if it is a separate third actor) before
// stopQuiescent/destruction. A timeout never authorizes freeing resources.
template<std::size_t MaxZones = 32, std::size_t MaxPlans = 8>
class LoopRealtimeSession {
public:
    using Storage = LoopRealtimeStorage<MaxZones>;
    using Phase = typename LoopProgramChannel<MaxPlans>::Phase;
private:
    class ControlReader final : public storage::RandomAccessReader {
    public:
        ControlReader(VerifiedReadOnlyBank& bank, const std::atomic<std::uint32_t>& stop,
                      LoopRealtimeConfig config)
            : bank_(bank), stop_(stop), config_(config) {}
        std::uint64_t size() const override { return bank_.size(); }
        bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
            if (!destination || offset > bank_.size() || bytes > bank_.size() - offset) return false;
            auto* output = static_cast<std::uint8_t*>(destination);
            do {
                if (stop_.load(std::memory_order_acquire)) { ++stats_.cancelled; return false; }
                const auto chunk = std::min<std::size_t>(bytes, 32768 - bytes_since_checkpoint_);
                const auto before = config_.now_us ? config_.now_us() : 0;
                if (!bank_.readAt(offset, output, chunk)) return false;
                if (config_.now_us) {
                    const auto after = config_.now_us();
                    const auto elapsed = after >= before ? after - before : 0;
                    stats_.read_total_us += elapsed;
                    stats_.read_max_us = std::max(stats_.read_max_us, elapsed);
                }
                ++stats_.reads; stats_.bytes += chunk;
                bytes_since_checkpoint_ += chunk; ++reads_since_checkpoint_;
                if (bytes_since_checkpoint_ >= 32768 || reads_since_checkpoint_ >= 64) {
                    bytes_since_checkpoint_ = 0; reads_since_checkpoint_ = 0;
                    ++stats_.checkpoints;
                    if (config_.io_checkpoint && !config_.io_checkpoint(config_.io_context)) {
                        ++stats_.cancelled; return false;
                    }
                }
                bytes -= chunk; offset += chunk; output += chunk;
            } while (bytes);
            return true;
        }
        const LoopRealtimeIoStats& stats() const { return stats_; }
    private:
        VerifiedReadOnlyBank& bank_;
        const std::atomic<std::uint32_t>& stop_;
        const LoopRealtimeConfig config_;
        LoopRealtimeIoStats stats_{};
        std::size_t bytes_since_checkpoint_ = 0, reads_since_checkpoint_ = 0;
    };
    struct Graph {
        ControlReader reader;
        sf2::Sf2PdtaReader pdta;
        sf2::Sf2SampleSource source;
        sample::SampleResidencyManager& residency;
        sample::PublishedSampleDirectory directory;
        sample::ProgramSnapshotExchange samples;
        gm::PlaybackProgramSnapshotExchange playback;
        sample::ProgramPrepareController controller{residency, directory, samples, &source};
        gm::RealtimeProgramChangeHandoff requests;
        gm::ProgramChangeMailbox mailbox;
        sf2::Sf2PlaybackProgramPreparer preparer;
        gm::ProgramChangeControlService control{requests, mailbox, pdta, preparer, controller, samples, playback};
        ResidentLoopProposalProvider provider;
        const LoopRealtimeConfig config;
        LoopRealtimeAnalysisTiming analysis_timing{};
        LoopProgramChannel<MaxPlans> channel;
        LoopProgramRuntime<MaxPlans> runtime{control, samples, playback};
        gm::RealtimeProgramEventApplier events{playback};
        gm::RealtimeProgramChangeRouter router{events, requests};
        voice::VoicePool pool{directory};
        realtime::RealtimeEventQueue queue;
        realtime::BlockEventScheduler scheduler{queue};
        realtime::ScheduledAudioBlockRenderer renderer{pool, scheduler, &gm::RealtimeProgramChangeRouter::callback, &router};
        bool installed = false;
        Graph(VerifiedReadOnlyBank& bank, sample::SampleResidencyManager& manager,
              const std::atomic<std::uint32_t>& stop, LoopRealtimeConfig settings)
            : reader(bank, stop, settings), residency(manager),
              provider(0, residency, directory, bank.asset(), settings.analysis), config(settings),
              channel(0, source, residency, directory, samples, playback, bank.asset(),
                      &Graph::proposal, this) {}
        static bool proposal(void* opaque, const loop::PreparedLoopProgram& program,
                             std::uint16_t zone, const loop::LoopPlanContext& context,
                             loop::LoopPlanRequest& output) noexcept {
            auto& self = *static_cast<Graph*>(opaque);
            const auto before = self.config.now_us ? self.config.now_us() : 0;
            const bool result = ResidentLoopProposalProvider::callback(&self.provider, program, zone, context, output);
            ++self.analysis_timing.calls;
            if (self.config.now_us) {
                const auto after = self.config.now_us();
                const auto elapsed = after >= before ? after - before : 0;
                self.analysis_timing.total_us += elapsed;
                self.analysis_timing.max_us = std::max(self.analysis_timing.max_us, elapsed);
            }
            return result;
        }
        bool shutdown() noexcept {
            pool.allSoundOff();
            if (installed) { runtime.uninstallQuiescent(events); installed = false; }
            if (!channel.resetQuiescent()) return false;
            const auto stats = directory.stats();
            return stats.live_voice_refs == 0 && stats.active_acquirers == 0 && stats.stale_release_faults == 0;
        }
    };
public:
    explicit LoopRealtimeSession(Storage& storage) : storage_(storage) {
        util::requireRealtimeAtomic32(stop_requested_);
        util::requireRealtimeAtomic32(audio_stopped_);
        util::requireRealtimeAtomic32(control_stopped_);
    }
    LoopRealtimeSession(const LoopRealtimeSession&) = delete;
    LoopRealtimeSession& operator=(const LoopRealtimeSession&) = delete;
    ~LoopRealtimeSession() { if (!stopQuiescent()) std::abort(); }

    LoopStartupStatus start(StartupLease incoming, const VerifiedReadOnlyBank::Digest& expected,
                            VerifiedReadOnlyBank::HashFn hash, StartupSerials& serials,
                            LoopRealtimeConfig config = {}) {
        if (status_ != LoopStartupStatus::Fresh) return LoopStartupStatus::AlreadyUsed;
        lease_ = std::move(incoming);
        if (!lease_.valid()) return fail(LoopStartupStatus::InvalidLease);
        serial_ = serials.next();
        if (!serial_) return fail(LoopStartupStatus::SerialExhausted);
        bank_.emplace();
        bank_status_ = bank_->open(lease_.bank(), lease_.bytes(), expected, serial_, 1, hash);
        if (bank_status_ != VerifiedReadOnlyBank::Status::Ok) return fail(LoopStartupStatus::BankRejected);
        if (storage_.residency_) return fail(LoopStartupStatus::ArenaFailed);
        storage_.residency_.emplace(); owns_storage_ = true;
        graph_.emplace(*bank_, *storage_.residency_, stop_requested_, config);
        auto& g = *graph_;
        if (!g.pdta.begin(g.reader) || !g.source.begin(g.reader)) return fail(LoopStartupStatus::ParseFailed);
        if (!g.residency.begin(lease_.arena(), lease_.arenaBytes())) return fail(LoopStartupStatus::ArenaFailed);
        sf2::PlaybackPrepareRequest request;
        request.channel = 0; request.bank = config.bank; request.program = config.program;
        request.token = 1; request.bank_generation = 1;
        auto& w = storage_.a.w;
        request.native_scratch = w.native_scratch; request.native_capacity = w.native_capacity;
        request.modulation_scratch = w.modulation_scratch; request.modulation_capacity = w.modulation_capacity;
        request.playback_zones = w.playback_zones; request.playback_capacity = w.playback_capacity;
        request.key_spans = w.key_spans; request.key_zone_indices = w.key_zone_indices;
        request.key_zone_index_capacity = w.key_zone_index_capacity;
        request.dependencies = w.dependencies; request.dependency_capacity = w.dependency_capacity;
        request.snapshot = &w.snapshot;
        const auto prepared = g.preparer.prepare(g.pdta, request);
        if (!prepared.ok()) return fail(LoopStartupStatus::PrepareFailed);
        if (!g.controller.activateInitialProgram(0, config.bank, config.program, 1,
                                               w.dependencies, prepared.dependency_count) ||
            !g.playback.setInitialActive(&w.snapshot) || !g.runtime.attach(g.channel))
            return fail(LoopStartupStatus::ActivationFailed);
        g.runtime.install(g.events); g.installed = true;
        if (g.channel.initializeActive() != loop::PreparedLoopStatus::Ok ||
            g.runtime.commitReadyAtBlockBoundary() != 1 || !g.runtime.acknowledgeCommitted(0).ok())
            return fail(LoopStartupStatus::ActivationFailed);
        status_ = LoopStartupStatus::Running;
        return status_;
    }

    // Called before publishing the session pointer to either task. Never rearm.
    bool beginActors() noexcept {
        if (status_ != LoopStartupStatus::Running || actors_started_ || stopRequested()) return false;
        actors_started_ = true;
        return true;
    }
    // ONE producer, before beginActors or on the control/input actor afterwards.
    // No read of audio-owned frame/status/Voice fields is made here.
    bool enqueue(realtime::RealtimeEvent event) noexcept {
        return graph_ && !stopRequested() && event.channel == 0 && graph_->queue.tryPush(event);
    }
    gm::ProgramChangeServiceStatus controlService() {
        using Result = gm::ProgramChangeServiceStatus;
        if (stopRequested()) { controlStop(); return Result::NoPending; }
        if (!actors_started_ || !graph_) return Result::NoActiveResidencySnapshot;
        auto& g = *graph_;
        if (g.channel.phase() == Phase::Committed && !g.runtime.acknowledgeCommitted(0).ok()) {
            requestStop(); controlStop(); return Result::AcknowledgeFailed;
        }
        g.runtime.drainRealtimeRequests();
        // Control can reuse only the inactive generation after the joint ACK.
        // While Ready, the immutable active pointer may change, but service's
        // phase gate rejects either workspace without dereferencing its data.
        auto& w = g.playback.active(0) == &storage_.a.w.snapshot ? storage_.b.w : storage_.a.w;
        const auto result = g.runtime.serviceChannel(0, w).status;
        if (g.channel.phase() == Phase::Fault) requestStop();
        if (stopRequested()) controlStop();
        return result;
    }
    bool audioRender(std::int16_t* stereo) {
        if (stopRequested()) { audioStop(); return false; }
        if (!actors_started_ || !graph_ || !stereo || frame_ > std::numeric_limits<std::uint64_t>::max() - 64) {
            requestStop(); audioStop(); return false;
        }
        graph_->runtime.render(graph_->renderer, frame_, stereo);
        frame_ += 64;
        if (graph_->channel.phase() == Phase::Fault) { requestStop(); audioStop(); return false; }
        return true;
    }
    void requestStop() noexcept { stop_requested_.store(1, std::memory_order_release); }
    bool stopRequested() const noexcept { return stop_requested_.load(std::memory_order_acquire) != 0; }
    bool audioStopped() const noexcept { return audio_stopped_.load(std::memory_order_acquire) != 0; }
    bool controlStopped() const noexcept { return control_stopped_.load(std::memory_order_acquire) != 0; }
    // Each ACK is the owner's LAST graph mutation. The coordinator must still
    // join the actual worker task/thread before destroying this object.
    void audioStop() noexcept {
        requestStop();
        if (!audioStopped()) {
            if (graph_) graph_->pool.allSoundOff();
            audio_stopped_.store(1, std::memory_order_release);
        }
    }
    void controlStop() noexcept {
        requestStop();
        control_stopped_.store(1, std::memory_order_release);
    }
    bool stopQuiescent() noexcept {
        if (actors_started_ && (!audioStopped() || !controlStopped())) return false;
        if (graph_) {
            if (!graph_->shutdown()) return false;
            final_publication_ = graph_->directory.stats();
            graph_.reset();
        }
        if (owns_storage_) { storage_.residency_.reset(); owns_storage_ = false; }
        bank_.reset(); lease_.reset();
        if (status_ == LoopStartupStatus::Running || status_ == LoopStartupStatus::Fresh)
            status_ = LoopStartupStatus::Stopped;
        return true;
    }

    // AUDIO owner only, or coordinator after joining audio. These are NOT
    // concurrent telemetry. Publish copies through a separately owned channel.
    std::uint64_t audioFrame() const { return frame_; }
    unsigned audioVoices() const { return graph_ ? graph_->pool.activeVoiceCount() : 0; }
    unsigned audioLoops() {
        unsigned count = 0;
        if (graph_) graph_->pool.updateActiveVoices(0, [](void* p, voice::Voice& v) {
            if (v.experimentalLoopEnabled()) ++*static_cast<unsigned*>(p);
            return true;
        }, &count);
        return count;
    }
    std::uint8_t audioProgram() const {
        const auto* program = graph_ ? graph_->playback.active(0) : nullptr;
        return program ? program->program : 255;
    }
    realtime::ScheduledAudioStats audioStats() const { return graph_ ? graph_->renderer.stats() : realtime::ScheduledAudioStats{}; }
    // CONTROL owner only, or coordinator after joining control.
    LoopStageReport controlStage() const { return graph_ ? graph_->channel.stageReport() : LoopStageReport{}; }
    ResidentLoopProviderReport controlAnalysis() const { return graph_ ? graph_->provider.report() : ResidentLoopProviderReport{}; }
    StartupAnalysisCounters controlBudget() const {
        if (!graph_) return {};
        const auto& b = graph_->provider.budget(); return {b.used, b.pcm_reads, b.remaining, b.failure};
    }
    sample::ResidencyStats controlResidencyStats() const { return graph_ ? graph_->residency.stats() : sample::ResidencyStats{}; }
    gm::ProgramChangeControlServiceStats controlServiceStats() const { return graph_ ? graph_->control.stats() : gm::ProgramChangeControlServiceStats{}; }
    LoopRealtimeIoStats controlIoStats() const { return graph_ ? graph_->reader.stats() : LoopRealtimeIoStats{}; }
    LoopRealtimeAnalysisTiming controlAnalysisTiming() const { return graph_ ? graph_->analysis_timing : LoopRealtimeAnalysisTiming{}; }
    // Atomic phase read is safe during either actor. All graph lifetime reads
    // still require callers to obey the stop/join ownership contract.
    Phase phase() const { return graph_ ? graph_->channel.phase() : Phase::Idle; }
    realtime::RealtimeEventQueueStats queueStats() const { return graph_ ? graph_->queue.stats() : realtime::RealtimeEventQueueStats{}; }
    sample::PublicationStats publicationStats() const { return graph_ ? graph_->directory.stats() : final_publication_; }
    std::uint64_t serial() const { return serial_; }
    LoopStartupStatus statusQuiescent() const { return status_; }
private:
    LoopStartupStatus fail(LoopStartupStatus status) {
        status_ = status;
        return stopQuiescent() ? status_ : LoopStartupStatus::Fault;
    }
    Storage& storage_;
    StartupLease lease_; // graph and verified reader are destroyed before PCM
    std::optional<VerifiedReadOnlyBank> bank_;
    std::atomic<std::uint32_t> stop_requested_{0}, audio_stopped_{0}, control_stopped_{0};
    std::optional<Graph> graph_;
    LoopStartupStatus status_ = LoopStartupStatus::Fresh;
    VerifiedReadOnlyBank::Status bank_status_ = VerifiedReadOnlyBank::Status::InvalidInput;
    bool actors_started_ = false, owns_storage_ = false;
    std::uint64_t serial_ = 0, frame_ = 0;
    sample::PublicationStats final_publication_{};
};
} // namespace gm_synth::app
#endif
