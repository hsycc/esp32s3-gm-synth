#pragma once
#include "app/ResidentLoopProposalProvider.h"
#include "app/VerifiedReadOnlyBank.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sf2/Sf2SampleSource.h"
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
namespace gm_synth::app {

// One serialized allocator per boot. IDs are burned on failure, never wrapped.
// No handle may survive reboot; this is NOT persistent OTA/session management.
class StartupSerials {
public:
    explicit StartupSerials(std::uint64_t last = 0) noexcept : last_(last) {}
    std::uint64_t next() noexcept {
        if (last_ == std::numeric_limits<std::uint64_t>::max()) return 0;
        return ++last_;
    }
private:
    std::uint64_t last_;
};

// Move-only ownership of BOTH immutable source and writable PCM arena. Backend
// must prevent writes/rebinds until release, and opaque must outlive this lease.
// No SD/OTA lock is inferred merely from a const pointer.
class StartupLease {
public:
    using Release = void (*)(void*) noexcept;
    StartupLease() = default;
    StartupLease(const std::uint8_t* bank, std::size_t bytes, void* arena,
                 std::size_t arena_bytes, void* opaque, Release release) noexcept
        : bank_(bank), bytes_(bytes), arena_(arena), arena_bytes_(arena_bytes),
          opaque_(opaque), release_(release) {}
    StartupLease(const StartupLease&) = delete;
    StartupLease& operator=(const StartupLease&) = delete;
    StartupLease(StartupLease&& other) noexcept { swap(other); }
    StartupLease& operator=(StartupLease&& other) noexcept {
        if (this != &other) { reset(); swap(other); }
        return *this;
    }
    ~StartupLease() { reset(); }
    bool valid() const noexcept {
        const auto b=reinterpret_cast<std::uintptr_t>(bank_);
        const auto a=reinterpret_cast<std::uintptr_t>(arena_);
        const auto max=std::numeric_limits<std::uintptr_t>::max();
        return bank_ && bytes_ && arena_ && arena_bytes_ && release_ &&
            bytes_<=max-b && arena_bytes_<=max-a &&
            (b+bytes_<=a || a+arena_bytes_<=b) &&
            a % sample::SampleResidencyManager::kAlignment == 0;
    }
    const std::uint8_t* bank() const noexcept { return bank_; }
    std::size_t bytes() const noexcept { return bytes_; }
    void* arena() const noexcept { return arena_; }
    std::size_t arenaBytes() const noexcept { return arena_bytes_; }
    void reset() noexcept {
        auto release = release_; auto opaque = opaque_;
        bank_ = nullptr; bytes_ = 0; arena_ = nullptr; arena_bytes_ = 0;
        opaque_ = nullptr; release_ = nullptr;
        if (release) release(opaque);
    }
private:
    void swap(StartupLease& other) noexcept {
        using std::swap;
        swap(bank_, other.bank_); swap(bytes_, other.bytes_); swap(arena_, other.arena_);
        swap(arena_bytes_, other.arena_bytes_); swap(opaque_, other.opaque_); swap(release_, other.release_);
    }
    const std::uint8_t* bank_ = nullptr;
    std::size_t bytes_ = 0;
    void* arena_ = nullptr;
    std::size_t arena_bytes_ = 0;
    void* opaque_ = nullptr;
    Release release_ = nullptr;
};

struct LoopStartupConfig {
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    // OFF by default, independently of compiling the optional Voice ABI.
    ResidentLoopAnalysisLimits analysis{};
};
struct StartupAnalysisCounters {
    std::uint32_t used=0, pcm_reads=0, remaining=0;
    LoopAnalysisStatus failure=LoopAnalysisStatus::Complete;
};
enum class LoopStartupStatus { Fresh, Running, Stopped, AlreadyUsed, InvalidLease, SerialExhausted,
    BankRejected, ParseFailed, ArenaFailed, PrepareFailed, ActivationFailed, Fault };

// An OWNED single-channel qualification session, called from ONE stopped or
// serialized task. It is not the concurrent control/audio actor API. All parser,
// snapshot, plan and Voice objects are private; no borrowed pointer escapes.
// Stop destroys the WHOLE group (including remaining Active Program pins), not
// live bank switching or recovery of the independent production controller.
template<std::size_t MaxZones = 32, std::size_t MaxPlans = 8>
class LoopStartupSession {
    static_assert(MaxZones > 0 && MaxZones <= 512 && MaxPlans > 0);
    struct Workspace {
        std::array<sf2::NativeResolvedZone, MaxZones> native{};
        std::array<sf2::ResolvedZoneModulation, MaxZones> mods{};
        std::array<gm::PlaybackZone, MaxZones> zones{};
        std::array<gm::PlaybackProgramKeySpan, 128> spans{};
        std::array<std::uint16_t, MaxZones * 128> indices{};
        std::array<sample::SampleId, MaxZones> deps{};
        gm::ProgramChangePrepareWorkspace w{};
        Workspace() {
            w.native_scratch=native.data(); w.native_capacity=native.size();
            w.modulation_scratch=mods.data(); w.modulation_capacity=mods.size();
            w.playback_zones=zones.data(); w.playback_capacity=zones.size();
            w.key_spans=spans.data(); w.key_zone_indices=indices.data(); w.key_zone_index_capacity=indices.size();
            w.dependencies=deps.data(); w.dependency_capacity=deps.size();
        }
    };
    struct Graph {
        sf2::Sf2PdtaReader pdta;
        sf2::Sf2SampleSource source;
        sample::SampleResidencyManager residency;
        sample::PublishedSampleDirectory directory;
        sample::ProgramSnapshotExchange samples;
        gm::PlaybackProgramSnapshotExchange playback;
        sample::ProgramPrepareController controller{residency,directory,samples,&source};
        gm::RealtimeProgramChangeHandoff requests;
        gm::ProgramChangeMailbox mailbox;
        sf2::Sf2PlaybackProgramPreparer preparer;
        gm::ProgramChangeControlService control{requests,mailbox,pdta,preparer,controller,samples,playback};
        Workspace a,b;
        ResidentLoopProposalProvider provider;
        LoopProgramChannel<MaxPlans> channel;
        LoopProgramRuntime<MaxPlans> runtime{control,samples,playback};
        gm::RealtimeProgramEventApplier events{playback};
        gm::RealtimeProgramChangeRouter router{events,requests};
        voice::VoicePool pool{directory};
        realtime::RealtimeEventQueue queue;
        realtime::BlockEventScheduler scheduler{queue};
        realtime::ScheduledAudioBlockRenderer renderer{pool,scheduler,&gm::RealtimeProgramChangeRouter::callback,&router};
        bool installed=false;
        Graph(loop::PreparedLoopAsset asset, ResidentLoopAnalysisLimits limits)
            : provider(0,residency,directory,asset,limits),
              channel(0,source,residency,directory,samples,playback,asset,
                      &ResidentLoopProposalProvider::callback,&provider) {}
        bool shutdown() noexcept {
            pool.allSoundOff();
            if (installed) { runtime.uninstallQuiescent(events); installed=false; }
            if (!channel.resetQuiescent()) return false;
            return directory.stats().live_voice_refs==0 && directory.stats().stale_release_faults==0;
        }
    };
public:
    LoopStartupSession() = default;
    LoopStartupSession(const LoopStartupSession&) = delete;
    LoopStartupSession& operator=(const LoopStartupSession&) = delete;
    ~LoopStartupSession() { if (!stop()) std::abort(); }

    // Always consumes the supplied lease. Failed/second starts release only the
    // newly supplied lease; they never replace a running group's resource.
    LoopStartupStatus start(StartupLease incoming, const VerifiedReadOnlyBank::Digest& expected,
                            VerifiedReadOnlyBank::HashFn hash, StartupSerials& serials,
                            LoopStartupConfig config = {}) {
        if (status_ != LoopStartupStatus::Fresh) return LoopStartupStatus::AlreadyUsed;
        lease_=std::move(incoming);
        if (!lease_.valid()) return fail(LoopStartupStatus::InvalidLease);
        serial_=serials.next();
        if (!serial_) return fail(LoopStartupStatus::SerialExhausted);
        bank_.emplace();
        // Generation is group-local. Globally non-reused serial disambiguates
        // separately constructed groups even when residency counters restart.
        bank_status_=bank_->open(lease_.bank(),lease_.bytes(),expected,serial_,1,hash);
        if (bank_status_!=VerifiedReadOnlyBank::Status::Ok) return fail(LoopStartupStatus::BankRejected);
        graph_.emplace(bank_->asset(),config.analysis);
        auto& g=*graph_;
        if (!g.pdta.begin(*bank_) || !g.source.begin(*bank_)) return fail(LoopStartupStatus::ParseFailed);
        if (!g.residency.begin(lease_.arena(),lease_.arenaBytes())) return fail(LoopStartupStatus::ArenaFailed);
        sf2::PlaybackPrepareRequest r;
        r.channel=0; r.bank=config.bank; r.program=config.program; r.token=1; r.bank_generation=1;
        const auto& w=g.a.w;
        r.native_scratch=w.native_scratch; r.native_capacity=w.native_capacity;
        r.modulation_scratch=w.modulation_scratch; r.modulation_capacity=w.modulation_capacity;
        r.playback_zones=w.playback_zones; r.playback_capacity=w.playback_capacity;
        r.key_spans=w.key_spans; r.key_zone_indices=w.key_zone_indices; r.key_zone_index_capacity=w.key_zone_index_capacity;
        r.dependencies=w.dependencies; r.dependency_capacity=w.dependency_capacity; r.snapshot=&g.a.w.snapshot;
        const auto prepared=g.preparer.prepare(g.pdta,r);
        if (!prepared.ok()) return fail(LoopStartupStatus::PrepareFailed);
        if (!g.controller.activateInitialProgram(0,config.bank,config.program,1,w.dependencies,prepared.dependency_count) ||
            !g.playback.setInitialActive(&g.a.w.snapshot) || !g.runtime.attach(g.channel))
            return fail(LoopStartupStatus::ActivationFailed);
        g.runtime.install(g.events); g.installed=true;
        if (g.channel.initializeActive()!=loop::PreparedLoopStatus::Ok ||
            g.runtime.commitReadyAtBlockBoundary()!=1 || !g.runtime.acknowledgeCommitted(0).ok())
            return fail(LoopStartupStatus::ActivationFailed);
        status_=LoopStartupStatus::Running;
        return status_;
    }
    bool enqueue(realtime::RealtimeEvent event) noexcept {
        return running() && event.channel==0 && event.timestamp_frame>=frame_ && graph_->queue.tryPush(event);
    }
    bool render(std::int16_t* stereo) {
        if (!running() || !stereo || frame_>std::numeric_limits<std::uint64_t>::max()-64) return false;
        graph_->runtime.render(graph_->renderer,frame_,stereo); frame_+=64;
        return graph_->channel.phase()!=LoopProgramChannel<MaxPlans>::Phase::Fault;
    }
    gm::ProgramChangeServiceStatus service() {
        if (!running()) return gm::ProgramChangeServiceStatus::NoActiveResidencySnapshot;
        auto& g=*graph_;
        // At most one group transaction; choose only the genuinely inactive
        // workspace. Wrapper gates reuse until acknowledgement has finished.
        if (g.channel.phase()==LoopProgramChannel<MaxPlans>::Phase::Committed &&
                !g.runtime.acknowledgeCommitted(0).ok()) { status_=LoopStartupStatus::Fault; return gm::ProgramChangeServiceStatus::AcknowledgeFailed; }
        g.runtime.drainRealtimeRequests();
        auto& w=g.playback.active(0)==&g.a.w.snapshot ? g.b.w : g.a.w;
        return g.runtime.serviceChannel(0,w).status;
    }
    bool stop() noexcept {
        if (graph_) {
            if (!graph_->shutdown()) { status_=LoopStartupStatus::Fault; return false; }
            graph_.reset(); // ALL snapshots/Program pins/records die before PCM
        }
        bank_.reset();
        lease_.reset();
        if (status_==LoopStartupStatus::Running || status_==LoopStartupStatus::Fresh) status_=LoopStartupStatus::Stopped;
        return true;
    }
    bool running() const noexcept { return status_==LoopStartupStatus::Running; }
    LoopStartupStatus status() const noexcept { return status_; }
    VerifiedReadOnlyBank::Status bankStatus() const noexcept { return bank_status_; }
    std::uint64_t serial() const noexcept { return serial_; }
    std::uint64_t frame() const noexcept { return frame_; }
    unsigned activeVoices() const { return graph_ ? graph_->pool.activeVoiceCount() : 0; }
    unsigned activeLoops() {
        unsigned count=0;
        if (graph_) graph_->pool.updateActiveVoices(0,[](void* p,voice::Voice& v){
            if(v.experimentalLoopEnabled()) ++*static_cast<unsigned*>(p);
            return true;},&count);
        return count;
    }
    std::uint8_t activeProgram() const { return graph_ && graph_->playback.active(0) ? graph_->playback.active(0)->program : 255; }
    LoopStageReport stage() const { return graph_ ? graph_->channel.stageReport() : LoopStageReport{}; }
    ResidentLoopProviderReport analysis() const { return graph_ ? graph_->provider.report() : ResidentLoopProviderReport{}; }
    StartupAnalysisCounters budget() const {
        if (!graph_) return {};
        const auto& b=graph_->provider.budget(); return {b.used,b.pcm_reads,b.remaining,b.failure};
    }
    sample::ResidencyStats residencyStats() const { return graph_ ? graph_->residency.stats() : sample::ResidencyStats{}; }
    realtime::ScheduledAudioStats audioStats() const { return graph_ ? graph_->renderer.stats() : realtime::ScheduledAudioStats{}; }
private:
    LoopStartupStatus fail(LoopStartupStatus status) {
        status_=status; if(!stop()) return LoopStartupStatus::Fault; return status_;
    }
    StartupLease lease_; // destroyed LAST
    std::optional<VerifiedReadOnlyBank> bank_;
    std::optional<Graph> graph_;
    LoopStartupStatus status_=LoopStartupStatus::Fresh;
    VerifiedReadOnlyBank::Status bank_status_=VerifiedReadOnlyBank::Status::InvalidInput;
    std::uint64_t serial_=0,frame_=0;
};
} // namespace gm_synth::app
#endif
