#pragma once

#include "app/BoundedLoopAnalyzer.h"
#include "app/LoopProgramRuntime.h"

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
namespace gm_synth::app {

struct ResidentLoopAnalysisLimits {
    bool experimental_opt_in = false;
    std::uint16_t max_analyses_per_program = 8;
    std::uint32_t max_work_per_program = 8*BoundedLoopAnalyzer::kMaxWorkPerAnalysis;
    // Source-loop fade is chosen by analysis. This separate output-frame bridge
    // is used only for UntilRelease; it never changes the Zone's loop mode.
    std::uint16_t release_bridge_frames = 32;
    LoopAnalysisBudget::Checkpoint checkpoint = nullptr;
    void* checkpoint_context = nullptr;
};
enum class ResidentLoopProviderStatus : std::uint8_t {
    Disabled, InvalidContext, Unavailable, StaleRecord, Budget, Cancelled,
    AnalysisFailed, Direct, Proposed, ReleaseFault, Reentrant
};
struct ResidentLoopProviderReport {
    std::uint32_t calls = 0, analyses = 0, completed = 0, proposed = 0;
    std::uint32_t context_rejected = 0, budget_skipped = 0;
    ResidentLoopProviderStatus last = ResidentLoopProviderStatus::Disabled;
    LoopAnalysisResult analysis{};
};

// Explicit control-only provider for ONE channel and ONE verified immutable
// bank/session. It reads the actual resident PCM under an independent temporary
// directory ref. No whole-sample copy, filesystem call, persistent cache or ID
// whitelist. Caller serializes source/Prepare mutations; checkpoints may yield,
// not mutate the program/PCM or recursively invoke this provider.
//
// The existing app transaction owns Program pins while the callback executes.
// Proposal output contains values only. Handoff independently re-derives and
// validates context on admission. All failures leave the optional entry absent
// (Direct); no partly scored candidate is promoted to a plan.
class ResidentLoopProposalProvider {
public:
    ResidentLoopProposalProvider(std::uint8_t channel,
        sample::SampleResidencyManager& residency, sample::PublishedSampleDirectory& directory,
        loop::PreparedLoopAsset asset, ResidentLoopAnalysisLimits limits = {}) noexcept
        : channel_(channel), residency_(residency), directory_(directory), asset_(asset), limits_(limits) {}
    ResidentLoopProposalProvider(const ResidentLoopProposalProvider&) = delete;
    ResidentLoopProposalProvider& operator=(const ResidentLoopProposalProvider&) = delete;

    static bool callback(void* opaque,const loop::PreparedLoopProgram& program,std::uint16_t zone,
                         const loop::LoopPlanContext& context,loop::LoopPlanRequest& out) noexcept {
        if (!opaque) { out={}; return false; }
        return static_cast<ResidentLoopProposalProvider*>(opaque)->propose(program,zone,context,out);
    }
    // Serialized control owner only. These reports are not atomic audio telemetry.
    const ResidentLoopProviderReport& report() const noexcept { return report_; }
    const LoopAnalysisBudget& budget() const noexcept { return budget_; }

    bool propose(const loop::PreparedLoopProgram& p,std::uint16_t zone,
                 const loop::LoopPlanContext& c,loop::LoopPlanRequest& out) noexcept {
        out={};
        if (busy_) { report_.last=ResidentLoopProviderStatus::Reentrant; return false; }
        busy_=true;
        struct Guard { bool& busy; ~Guard(){busy=false;} } guard{busy_};
        report_.analysis={};
        if (!limits_.experimental_opt_in) { report_.last=ResidentLoopProviderStatus::Disabled; return false; }
        if (!programValid(p) || !limitsValid()) {
            ++report_.context_rejected; report_.last=ResidentLoopProviderStatus::InvalidContext; return false;
        }
        // Full transaction identity, not only Sample ID. Repeated calls for the
        // same transaction share the budget; new tokens reset it once.
        if (!sameProgram(p)) {
            program_=p; report_={}; budget_={};
            budget_.remaining=limits_.max_work_per_program;
            budget_.checkpoint=limits_.checkpoint; budget_.opaque=limits_.checkpoint_context;
        }
        ++report_.calls; report_.analysis={};
        if (zone>=p.playback->zone_count || !contextValid(p,zone,c)) {
            ++report_.context_rejected; report_.last=ResidentLoopProviderStatus::InvalidContext; return false;
        }
        if (report_.analyses>=limits_.max_analyses_per_program || budget_.remaining==0 ||
                budget_.failure!=LoopAnalysisStatus::Complete) {
            ++report_.budget_skipped;
            report_.last=budget_.failure==LoopAnalysisStatus::Cancelled ?
                ResidentLoopProviderStatus::Cancelled : ResidentLoopProviderStatus::Budget;
            return false;
        }
        auto pin=directory_.acquire(c.sample_id);
        if (!pin.valid()) { report_.last=ResidentLoopProviderStatus::Unavailable; return false; }
        if (!recordMatches(pin,c,p.playback->zones[zone])) {
            const bool released=directory_.release(pin);
            report_.last=released ? ResidentLoopProviderStatus::StaleRecord : ResidentLoopProviderStatus::ReleaseFault;
            return false;
        }
        ++report_.analyses;
        const auto status=analyzer_.analyze(pin.record->pcm+c.region.loop_start,
            c.region.loop_end-c.region.loop_start,budget_,report_.analysis);
        const bool still_current=recordMatches(pin,c,p.playback->zones[zone]) &&
            directory_.published(c.sample_id)==pin.record && p.matches(p.playback,p.residency);
        if (!directory_.release(pin)) {
            report_.analysis={}; report_.last=ResidentLoopProviderStatus::ReleaseFault; return false;
        }
        if (!still_current) { report_.analysis={}; report_.last=ResidentLoopProviderStatus::StaleRecord; return false; }
        if (status!=LoopAnalysisStatus::Complete) {
            report_.last=status==LoopAnalysisStatus::Budget ? ResidentLoopProviderStatus::Budget :
                status==LoopAnalysisStatus::Cancelled ? ResidentLoopProviderStatus::Cancelled :
                ResidentLoopProviderStatus::AnalysisFailed;
            return false;
        }
        ++report_.completed;
        if (!report_.analysis.selected_fade) { report_.last=ResidentLoopProviderStatus::Direct; return false; }
        out.context=c; out.experimental_opt_in=true;
        out.fade={true,report_.analysis.selected_fade};
        if (c.mode==loop::CursorLoopMode::UntilRelease && limits_.release_bridge_frames)
            out.release={true,limits_.release_bridge_frames};
        ++report_.proposed; report_.last=ResidentLoopProviderStatus::Proposed;
        return true;
    }
private:
    bool limitsValid() const noexcept {
        const auto r=limits_.release_bridge_frames;
        return limits_.max_analyses_per_program<=64 && limits_.max_work_per_program<=3200000 &&
            (r==0 || r==16 || r==32 || r==64);
    }
    bool programValid(const loop::PreparedLoopProgram& p) const noexcept {
        return channel_<16 && asset_.valid() && p.asset.sha256==asset_.sha256 &&
            p.asset.session==asset_.session && p.asset.bank_generation==asset_.bank_generation &&
            p.bank_generation==asset_.bank_generation && p.channel==channel_ && p.token &&
            p.matches(p.playback,p.residency) && p.playback->valid() && p.residency->valid();
    }
    bool sameProgram(const loop::PreparedLoopProgram& p) const noexcept {
        return program_.matches(p.playback,p.residency) &&
            program_.asset.sha256==p.asset.sha256 && program_.asset.session==p.asset.session;
    }
    bool contextValid(const loop::PreparedLoopProgram& p,std::uint16_t zone,
                      const loop::LoopPlanContext& c) const noexcept {
        if (c.asset_sha256!=asset_.sha256 || c.session_serial!=asset_.session ||
            c.bank_generation!=asset_.bank_generation || !c.sample_generation || !c.sample_rate ||
            c.sample_type!=1 || !c.noteon_addresses_fixed || c.policy_revision!=3 || c.coordinate_revision!=1 ||
            (c.mode!=loop::CursorLoopMode::Continuous && c.mode!=loop::CursorLoopMode::UntilRelease)) return false;
        const auto& r=c.region;
        if (!(r.start<r.end && r.end<=c.sample_frames && r.start<=r.loop_start &&
              r.loop_start<r.loop_end && r.loop_end<=r.end && r.loop_end-r.loop_start>=4)) return false;
        const auto& z=p.playback->zones[zone];
        if (z.sample_id!=c.sample_id || !z.valid() ||
            (z.loop_mode!=voice::LoopMode::Continuous && z.loop_mode!=voice::LoopMode::UntilRelease) ||
            (z.loop_mode==voice::LoopMode::Continuous)!=(c.mode==loop::CursorLoopMode::Continuous)) return false;
        if (z.sample_region.enabled && (r.start!=z.sample_region.start_frame || r.end!=z.sample_region.end_frame ||
            r.loop_start!=z.sample_region.loop_start_frame || r.loop_end!=z.sample_region.loop_end_frame)) return false;
        bool found=false;
        for (std::size_t i=0;i<p.residency->dependency_count;++i)
            found=found || p.residency->sample_ids[i]==c.sample_id;
        // Bound metadata loops by production snapshot capacities. PCM analysis
        // is separately budgeted; these are existing prepared-zone metadata.
        for (std::size_t i=0;i<z.prepared_note_on_modulators.count;++i) {
            const auto d=z.prepared_note_on_modulators.routes[i].destination_oper;
            if (d==0 || d==1 || d==2 || d==3 || d==4 || d==12 || d==45 || d==50) return false;
        }
        return found;
    }
    bool recordMatches(const sample::PublishedVoiceRef& pin,const loop::LoopPlanContext& c, const gm::PlaybackZone& zone) const noexcept {
        if (!pin.valid() || pin.sample_id!=c.sample_id || pin.bank_generation!=c.bank_generation ||
            pin.sample_generation!=c.sample_generation) return false;
        const auto& r=*pin.record; const auto& d=r.descriptor;
        const auto view=residency_.residentView(c.sample_id);
        if (!zone.sample_region.enabled && (c.region.start!=0 || c.region.end!=d.frames ||
            c.region.loop_start!=d.loop_start || c.region.loop_end!=d.loop_end)) return false;
        return directory_.bankGeneration()==c.bank_generation && view.valid() && view.pin_count>0 && view.generation==c.sample_generation &&
            view.pcm==r.pcm && view.descriptor==d && r.pcm && d.id==c.sample_id &&
            r.bank_generation==c.bank_generation && r.sample_generation==c.sample_generation &&
            d.frames==c.sample_frames && d.sample_rate==c.sample_rate && d.flags==1 &&
            std::uint64_t{d.pcm_bytes}==std::uint64_t{d.frames}*2;
    }
    const std::uint8_t channel_;
    sample::SampleResidencyManager& residency_;
    sample::PublishedSampleDirectory& directory_;
    const loop::PreparedLoopAsset asset_;
    const ResidentLoopAnalysisLimits limits_;
    loop::PreparedLoopProgram program_{};
    LoopAnalysisBudget budget_{};
    ResidentLoopProviderReport report_{};
    BoundedLoopAnalyzer analyzer_{};
    bool busy_=false;
};
} // namespace gm_synth::app
#endif
