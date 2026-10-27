#pragma once
#include "app/ResidentLoopProposalProvider.h"
#include "app/VerifiedReadOnlyBank.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sf2/Sf2SampleSource.h"
#include "loop_analysis_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <new>

namespace loop_analysis_test {
using namespace gm_synth;
namespace x=voice::experimental;
using Bank=app::VerifiedReadOnlyBank;
inline void require(bool ok,int line) {
    if(!ok){std::printf("ANALYSIS_FAIL line=%d\n",line);std::fflush(stdout);std::abort();}
}
#define ANALYSIS_CHECK(x) ::loop_analysis_test::require(bool(x),__LINE__)
struct Workspace {
    std::array<sf2::NativeResolvedZone,4> native{};
    std::array<sf2::ResolvedZoneModulation,4> mods{};
    std::array<gm::PlaybackZone,4> zones{};
    std::array<gm::PlaybackProgramKeySpan,128> spans{};
    std::array<std::uint16_t,512> indexes{};
    std::array<sample::SampleId,4> deps{};
    gm::ProgramChangePrepareWorkspace w{};
    Workspace() {
        w.native_scratch=native.data();w.native_capacity=native.size();
        w.modulation_scratch=mods.data();w.modulation_capacity=mods.size();
        w.playback_zones=zones.data();w.playback_capacity=zones.size();
        w.key_spans=spans.data();w.key_zone_indices=indexes.data();w.key_zone_index_capacity=indexes.size();
        w.dependencies=deps.data();w.dependency_capacity=deps.size();
    }
};
struct Checkpoints {
    void (*yield)()=nullptr;
    bool cancel=false;
    unsigned calls=0;
    static bool run(void* p) {
        auto& self=*static_cast<Checkpoints*>(p);++self.calls;
        if(self.yield)self.yield();
        return !self.cancel;
    }
};
struct Fixture {
    Bank reader;
    sf2::Sf2PdtaReader pdta; sf2::Sf2SampleSource source;
    std::array<std::uint32_t,5120> arena{};
    sample::SampleResidencyManager residency;
    sample::PublishedSampleDirectory directory;
    sample::ProgramSnapshotExchange samples;
    gm::PlaybackProgramSnapshotExchange playback;
    sample::ProgramPrepareController controller{residency,directory,samples,&source};
    gm::RealtimeProgramChangeHandoff requests;gm::ProgramChangeMailbox mailbox;
    sf2::Sf2PlaybackProgramPreparer preparer;
    gm::ProgramChangeControlService control{requests,mailbox,pdta,preparer,controller,samples,playback};
    Workspace a,b;
    Checkpoints checkpoints;
    alignas(app::ResidentLoopProposalProvider) std::array<std::uint8_t,sizeof(app::ResidentLoopProposalProvider)> provider_storage{};
    alignas(app::LoopProgramChannel<2>) std::array<std::uint8_t,sizeof(app::LoopProgramChannel<2>)> channel_storage{};
    app::ResidentLoopProposalProvider* provider=nullptr;
    app::LoopProgramChannel<2>* channel=nullptr;
    app::LoopProgramRuntime<2> runtime{control,samples,playback};
    gm::RealtimeProgramEventApplier events{playback};
    gm::RealtimeProgramChangeRouter router{events,requests};
    voice::VoicePool pool{directory};
    realtime::RealtimeEventQueue queue;realtime::BlockEventScheduler scheduler{queue};
    realtime::ScheduledAudioBlockRenderer renderer{pool,scheduler,&gm::RealtimeProgramChangeRouter::callback,&router};
    std::array<std::int16_t,128> output{};
    std::uint64_t hash=UINT64_C(14695981039346656037),frame=0;
    void begin(Bank::HashFn hash_fn,app::ResidentLoopAnalysisLimits limits,void(*yield)()) {
        checkpoints.yield=yield;limits.checkpoint=&Checkpoints::run;limits.checkpoint_context=&checkpoints;
        ANALYSIS_CHECK(reader.open(loop_analysis_fixture::bytes,sizeof(loop_analysis_fixture::bytes),
            loop_analysis_fixture::digest,1,1,hash_fn)==Bank::Status::Ok);
        ANALYSIS_CHECK(pdta.begin(reader) && source.begin(reader));
        ANALYSIS_CHECK(residency.begin(arena.data(),sizeof(arena)));
        sf2::PlaybackPrepareRequest r;
        r.channel=0;r.bank=0;r.program=0;r.token=100;r.bank_generation=1;
        r.native_scratch=a.w.native_scratch;r.native_capacity=a.w.native_capacity;
        r.modulation_scratch=a.w.modulation_scratch;r.modulation_capacity=a.w.modulation_capacity;
        r.playback_zones=a.w.playback_zones;r.playback_capacity=a.w.playback_capacity;
        r.key_spans=a.w.key_spans;r.key_zone_indices=a.w.key_zone_indices;r.key_zone_index_capacity=a.w.key_zone_index_capacity;
        r.dependencies=a.w.dependencies;r.dependency_capacity=a.w.dependency_capacity;r.snapshot=&a.w.snapshot;
        const auto prepared=preparer.prepare(pdta,r);ANALYSIS_CHECK(prepared.ok() && a.w.snapshot.zone_count==3);
        ANALYSIS_CHECK(controller.activateInitialProgram(0,0,0,100,a.w.dependencies,prepared.dependency_count));
        ANALYSIS_CHECK(playback.setInitialActive(&a.w.snapshot));
        provider=new(provider_storage.data()) app::ResidentLoopProposalProvider(0,residency,directory,reader.asset(),limits);
        channel=new(channel_storage.data()) app::LoopProgramChannel<2>(0,source,residency,directory,samples,
            playback,reader.asset(),&app::ResidentLoopProposalProvider::callback,provider);
        ANALYSIS_CHECK(runtime.attach(*channel));runtime.install(events);
    }
    void initialize() {
        ANALYSIS_CHECK(channel->initializeActive()==x::PreparedLoopStatus::Ok);
        ANALYSIS_CHECK(runtime.commitReadyAtBlockBoundary()==1);ANALYSIS_CHECK(runtime.acknowledgeCommitted(0).ok());
    }
    realtime::RealtimeEvent event(realtime::EventType type,unsigned key,unsigned velocity=0) {
        realtime::RealtimeEvent e{};e.type=type;e.channel=0;e.data1=key;e.data2=velocity;e.timestamp_frame=frame;return e;
    }
    unsigned activeLoops() {
        unsigned count=0;
        pool.updateActiveVoices(0,[](void* p,voice::Voice& v){if(v.experimentalLoopEnabled())++*static_cast<unsigned*>(p);return true;},&count);
        return count;
    }
    void render() {
        runtime.render(renderer,frame,output.data());frame+=64;
        for(auto v:output){hash^=static_cast<std::uint16_t>(v);hash*=UINT64_C(1099511628211);}
        hash^=pool.activeVoiceCount();hash*=UINT64_C(1099511628211);
    }
    void close() {
        pool.allSoundOff();runtime.uninstallQuiescent(events);
        ANALYSIS_CHECK(channel->resetQuiescent());channel->~LoopProgramChannel();provider->~ResidentLoopProposalProvider();
        ANALYSIS_CHECK(directory.stats().live_voice_refs==0 && directory.stats().stale_release_faults==0);
    }
};

inline void run(Bank::HashFn hash_fn,void(*yield)()=nullptr) {
    alignas(Fixture) static std::uint8_t storage[sizeof(Fixture)];
    unsigned suites=0;std::uint64_t trace=0;
    app::ResidentLoopAnalysisLimits limits;limits.experimental_opt_in=true;
    auto* f=new(storage) Fixture;f->begin(hash_fn,limits,yield);f->initialize();
    ANALYSIS_CHECK(f->channel->stageReport().admitted==2 && f->channel->stageReport().capacity_skipped==1);
    ANALYSIS_CHECK(f->provider->report().completed==2 && f->provider->report().analysis.selected_fade==32);
    ANALYSIS_CHECK(f->checkpoints.calls>0);
    const auto work=f->provider->budget().used,reads=f->provider->budget().pcm_reads;
    ANALYSIS_CHECK(work<=2*app::BoundedLoopAnalyzer::kMaxWorkPerAnalysis && reads>0 && reads<=4000);
    ANALYSIS_CHECK(f->router.apply(f->event(realtime::EventType::NoteOn,60,100),f->pool));
    ANALYSIS_CHECK(f->activeLoops()==1);
    for(unsigned i=0;i<8;++i)f->render();
    ANALYSIS_CHECK(f->provider->budget().used==work && f->provider->budget().pcm_reads==reads);
    const auto* old=f->directory.published(0);ANALYSIS_CHECK(old);
    ANALYSIS_CHECK(f->router.apply(f->event(realtime::EventType::ProgramChange,1),f->pool));
    ANALYSIS_CHECK(f->runtime.drainRealtimeRequests()==1 && f->runtime.serviceChannel(0,f->b.w).ok());
    ANALYSIS_CHECK(f->channel->stageReport().admitted==0); // smooth replacement is Direct, not an ID map
    ANALYSIS_CHECK(f->provider->report().completed==1 && f->provider->report().analysis.selected_fade==0);
    ANALYSIS_CHECK(f->queue.tryPush(f->event(realtime::EventType::NoteOn,60,100)));
    f->render(); // joint commit before timestamp-zero NoteOn
    ANALYSIS_CHECK(f->playback.active(0)==&f->b.w.snapshot && f->activeLoops()==1 && f->pool.activeVoiceCount()==2);
    ANALYSIS_CHECK(f->runtime.acknowledgeCommitted(0).ok());
    ANALYSIS_CHECK(f->directory.published(0)==nullptr && !f->directory.canReclaim(old));
    for(unsigned i=0;i<7;++i)f->render();
    trace=f->hash;
    f->pool.allSoundOff();ANALYSIS_CHECK(f->directory.canReclaim(old) && f->residency.evict(0));
    f->close();f->~Fixture();++suites;

    // Disabled, per-program analysis cap, work cap and cooperative cancellation
    // all leave a valid Program active and preserve ordinary Direct starts.
    for(unsigned kind=0;kind<4;++kind) {
        limits={};limits.experimental_opt_in=kind!=0;
        if(kind==1)limits.max_analyses_per_program=1;
        if(kind==2)limits.max_work_per_program=100;
        f=new(storage) Fixture;f->begin(hash_fn,limits,yield);f->checkpoints.cancel=kind==3;f->initialize();
        ANALYSIS_CHECK(f->channel->stageReport().admitted==(kind==1?1u:0u));
        ANALYSIS_CHECK(f->router.apply(f->event(realtime::EventType::NoteOn,61,100),f->pool));
        ANALYSIS_CHECK(f->activeLoops()==0 && f->pool.activeVoiceCount()==1);
        if(kind==0)ANALYSIS_CHECK(f->provider->budget().used==0);
        if(kind==1)ANALYSIS_CHECK(f->provider->report().completed==1 && f->provider->report().budget_skipped==2);
        if(kind==2)ANALYSIS_CHECK(f->provider->budget().failure==app::LoopAnalysisStatus::Budget);
        if(kind==3)ANALYSIS_CHECK(f->provider->budget().failure==app::LoopAnalysisStatus::Cancelled);
        f->close();f->~Fixture();++suites;
    }
    limits={};limits.experimental_opt_in=true;
    f=new(storage) Fixture;f->begin(hash_fn,limits,yield);f->initialize();
    x::PreparedLoopContextBuilder builder;
    ANALYSIS_CHECK(builder.begin(f->source,f->residency,f->directory,*f->playback.active(0),
        *f->samples.active(0),f->reader.asset())==x::PreparedLoopStatus::Ok);
    x::LoopPlanContext c;ANALYSIS_CHECK(builder.derive(0,c)==x::PreparedLoopStatus::Ok);
    const auto before=f->directory.stats().live_voice_refs;
    for(unsigned kind=0;kind<6;++kind) {
        auto bad=c;auto program=builder.program();
        if(kind==0)++bad.sample_generation;
        if(kind==1)++bad.region.loop_start;
        if(kind==2)++bad.policy_revision;
        if(kind==3)bad.asset_sha256[0]^=1;
        if(kind==4)++program.token;
        if(kind==5)bad.noteon_addresses_fixed=false;
        x::LoopPlanRequest out;out.experimental_opt_in=true;out.fade={true,64};
        ANALYSIS_CHECK(!f->provider->propose(program,0,bad,out) && !out.experimental_opt_in && !out.fade.frames);
        ANALYSIS_CHECK(f->directory.stats().live_voice_refs==before);
    }
    f->close();f->~Fixture();++suites;
    std::printf("ANALYSIS_RESULT suites=%u frames=1024 trace=%" PRIu64 " initial_fade=32 replacement_fade=0 work=%u reads=%u\n",
        suites,trace,unsigned(work),unsigned(reads));
    std::printf("ANALYSIS_LAYOUT pointer=%u analyzer=%u provider=%u fixture=%u\n",
        unsigned(sizeof(void*)),unsigned(sizeof(app::BoundedLoopAnalyzer)),
        unsigned(sizeof(app::ResidentLoopProposalProvider)),unsigned(sizeof(Fixture)));
    std::printf("ANALYSIS_ASSET sha256=%s\nANALYSIS_PASS\n",loop_analysis_fixture::digest_hex);
    std::fflush(stdout);
}
#undef ANALYSIS_CHECK
} // namespace loop_analysis_test
