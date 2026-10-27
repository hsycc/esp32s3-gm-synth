#pragma once
#include "app/LoopProgramRuntime.h"
#include "app/VerifiedReadOnlyBank.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sf2/Sf2SampleSource.h"
#include "loop_app_fixture.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP != 1
#error Application qualification requires build-wide enabled Voice ABI
#endif
namespace loop_app_target {
using namespace gm_synth;
namespace x = voice::experimental;
using Bank = app::VerifiedReadOnlyBank;
inline void require(bool ok, int line) {
    if (!ok) { std::printf("V1_APP_FAIL line=%d\n", line); std::fflush(stdout); std::abort(); }
}
#define APP_REQUIRE(v) ::loop_app_target::require(bool(v), __LINE__)
using Yield = void (*)();
struct Worker {
    void (*start)(void (*)(void*), void*);
    void (*join)();
    Yield yield;
};
struct Workspace {
    std::array<sf2::NativeResolvedZone,4> native{};
    std::array<sf2::ResolvedZoneModulation,4> mods{};
    std::array<gm::PlaybackZone,4> zones{};
    std::array<gm::PlaybackProgramKeySpan,128> spans{};
    std::array<std::uint16_t,4*128> indexes{};
    std::array<sample::SampleId,4> deps{};
    gm::ProgramChangePrepareWorkspace w{};
    Workspace() {
        w.native_scratch=native.data(); w.native_capacity=native.size();
        w.modulation_scratch=mods.data(); w.modulation_capacity=mods.size();
        w.playback_zones=zones.data(); w.playback_capacity=zones.size();
        w.key_spans=spans.data(); w.key_zone_indices=indexes.data(); w.key_zone_index_capacity=indexes.size();
        w.dependencies=deps.data(); w.dependency_capacity=deps.size();
    }
};
struct Provider {
    bool decline=false, wrong=false;
    std::atomic<bool> pause{false}, entered{false}, resume{false};
    Yield yield=nullptr;
    static bool propose(void* opaque,const x::PreparedLoopProgram&,std::uint16_t,
                        const x::LoopPlanContext& context,x::LoopPlanRequest& out) {
        auto& p=*static_cast<Provider*>(opaque);
        if(p.pause.load(std::memory_order_acquire)) {
            p.entered.store(true,std::memory_order_release);
            while(!p.resume.load(std::memory_order_acquire)) p.yield();
        }
        if(p.decline || context.mode==x::CursorLoopMode::None) return false;
        out.context=context; out.experimental_opt_in=true; out.fade={true,32}; out.release={true,32};
        if(p.wrong) ++out.context.session_serial;
        return true; // synthetic transport fixture ONLY, NOT a quality policy
    }
};
struct Fixture {
    Bank reader;
    sf2::Sf2PdtaReader pdta;
    sf2::Sf2SampleSource source;
    std::array<std::uint32_t,4096> arena{};
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
    Provider provider;
    alignas(app::LoopProgramChannel<2>) std::array<std::uint8_t,sizeof(app::LoopProgramChannel<2>)> channel_storage{};
    app::LoopProgramChannel<2>* channel=nullptr;
    app::LoopProgramRuntime<2> runtime{control,samples,playback};
    gm::RealtimeProgramEventApplier events{playback};
    gm::RealtimeProgramChangeRouter router{events,requests};
    voice::VoicePool voices{directory};
    realtime::RealtimeEventQueue queue;
    realtime::BlockEventScheduler scheduler{queue};
    realtime::ScheduledAudioBlockRenderer renderer{voices,scheduler,&gm::RealtimeProgramChangeRouter::callback,&router};
    std::array<std::int16_t,realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> pcm{};
    std::uint64_t frame=0, hash=UINT64_C(14695981039346656037);
    unsigned commits=0;
    void begin(Bank::HashFn hash_fn,std::uint64_t session=1) {
        APP_REQUIRE(reader.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),
            loop_app_fixture::digest,session,1,hash_fn)==Bank::Status::Ok);
        APP_REQUIRE(pdta.begin(reader) && source.begin(reader));
        APP_REQUIRE(residency.begin(arena.data(),sizeof(arena)));
        sf2::PlaybackPrepareRequest r{};
        r.channel=0;r.bank=0;r.program=0;r.token=100;r.bank_generation=1;
        r.native_scratch=a.w.native_scratch;r.native_capacity=a.w.native_capacity;
        r.modulation_scratch=a.w.modulation_scratch;r.modulation_capacity=a.w.modulation_capacity;
        r.playback_zones=a.w.playback_zones;r.playback_capacity=a.w.playback_capacity;
        r.key_spans=a.w.key_spans;r.key_zone_indices=a.w.key_zone_indices;r.key_zone_index_capacity=a.w.key_zone_index_capacity;
        r.dependencies=a.w.dependencies;r.dependency_capacity=a.w.dependency_capacity;r.snapshot=&a.w.snapshot;
        auto prepared=preparer.prepare(pdta,r); APP_REQUIRE(prepared.ok() && a.w.snapshot.zone_count==3);
        APP_REQUIRE(controller.activateInitialProgram(0,0,0,100,a.w.dependencies,prepared.dependency_count));
        APP_REQUIRE(playback.setInitialActive(&a.w.snapshot));
        channel=new(channel_storage.data()) app::LoopProgramChannel<2>(0,source,residency,directory,samples,
            playback,reader.asset(),&Provider::propose,&provider);
        APP_REQUIRE(runtime.attach(*channel));runtime.install(events);
        APP_REQUIRE(channel->initializeActive()==x::PreparedLoopStatus::Ok);
        APP_REQUIRE(!runtime.acknowledgeCommitted(0).ok());
        APP_REQUIRE(runtime.commitReadyAtBlockBoundary()==1);++commits;
        APP_REQUIRE(runtime.acknowledgeCommitted(0).ok());
    }
    void close() {
        voices.allSoundOff();runtime.uninstallQuiescent(events);
        APP_REQUIRE(channel->resetQuiescent());channel->~LoopProgramChannel();channel=nullptr;
        APP_REQUIRE(directory.stats().live_voice_refs==0 && directory.stats().stale_release_faults==0);
    }
    realtime::RealtimeEvent event(realtime::EventType type,unsigned d1,unsigned d2=0,unsigned offset=0) const {
        realtime::RealtimeEvent e{};e.type=type;e.channel=0;e.data1=d1;e.data2=d2;e.timestamp_frame=frame+offset;return e;
    }
    void push(realtime::EventType type,unsigned d1,unsigned d2=0,unsigned offset=0) {
        APP_REQUIRE(queue.tryPush(event(type,d1,d2,offset)));
    }
    void render() {
        runtime.render(renderer,frame,pcm.data());
        for(auto value:pcm){hash^=static_cast<std::uint16_t>(value);hash*=UINT64_C(1099511628211);}
        hash^=voices.activeVoiceCount();hash*=UINT64_C(1099511628211);
        frame+=64;
    }
    unsigned loops() {
        unsigned n=0;voices.updateActiveVoices(0,[](void* v,voice::Voice& voice){
            if(voice.experimentalLoopEnabled()) { ++*static_cast<unsigned*>(v); }
            return true;},&n);return n;
    }
    void request(unsigned program) {
        push(realtime::EventType::ProgramChange,program);render();
        APP_REQUIRE(runtime.drainRealtimeRequests()==1);
    }
};

inline void identity_tests(Bank::HashFn hash_fn) {
    auto expected=loop_app_fixture::digest;
    Bank b;std::array<std::uint8_t,12> header{};
    APP_REQUIRE(!b.valid() && b.size()==0 && !b.readAt(0,header.data(),1));
    APP_REQUIRE(b.open(nullptr,0,expected,1,1,hash_fn)==Bank::Status::InvalidInput);
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),expected,0,1,hash_fn)==Bank::Status::InvalidInput);
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),expected,1,0,hash_fn)==Bank::Status::InvalidInput);
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),expected,1,1,hash_fn,32)==Bank::Status::InvalidInput);
    auto wrong=expected;wrong[0]^=1;
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),wrong,1,1,hash_fn)==Bank::Status::DigestMismatch);
    const auto failure=[](const std::uint8_t*,std::size_t,Bank::Digest&)noexcept{return false;};
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),expected,1,1,failure)==Bank::Status::HashFailed);
    static std::array<std::uint8_t,sizeof(loop_app_fixture::bytes)> corrupt{};
    std::copy_n(loop_app_fixture::bytes,corrupt.size(),corrupt.data());corrupt[100]^=1;
    APP_REQUIRE(b.open(corrupt.data(),corrupt.size(),expected,1,1,hash_fn)==Bank::Status::DigestMismatch);
    corrupt[0]='X'; APP_REQUIRE(b.open(corrupt.data(),corrupt.size(),expected,1,1,hash_fn)==Bank::Status::InvalidContainer);
    APP_REQUIRE(!b.valid() && !b.asset().valid());
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),expected,41,1,hash_fn)==Bank::Status::Ok);
    APP_REQUIRE(b.asset().sha256==expected && b.asset().session==41);
    APP_REQUIRE(b.open(loop_app_fixture::bytes,sizeof(loop_app_fixture::bytes),wrong,42,2,hash_fn)==Bank::Status::AlreadyOpen);
    APP_REQUIRE(b.asset().session==41 && b.asset().bank_generation==1);
    APP_REQUIRE(b.readAt(0,header.data(),header.size()) && std::memcmp(header.data(),"RIFF",4)==0);
    APP_REQUIRE(!b.readAt(std::numeric_limits<std::uint64_t>::max(),header.data(),1));
    APP_REQUIRE(!b.readAt(b.size(),header.data(),1));
    APP_REQUIRE(!b.readAt(0,header.data(),std::numeric_limits<std::size_t>::max()));
    APP_REQUIRE(b.readAt(b.size(),header.data(),0));
    std::puts("APP_TARGET_IDENTITY PASS checks=19 computed_sha256=1 rebind_rejected=1");
}

inline void run(Bank::HashFn hash_fn,Worker worker) {
    identity_tests(hash_fn);
    // Large fixed storage is static test memory, not the FreeRTOS task stack.
    alignas(Fixture) static std::array<std::uint8_t,sizeof(Fixture)> storage{};
    auto* f=new(storage.data()) Fixture;
    f->begin(hash_fn);
    APP_REQUIRE(f->channel->stageReport().admitted==2 && f->channel->stageReport().capacity_skipped==1);
    using E=realtime::EventType;
    f->push(E::NoteOn,60,100);f->push(E::NoteOn,60,50,8);f->push(E::NoteOn,61,90,16);f->render();
    APP_REQUIRE(f->voices.activeVoiceCount()==3 && f->loops()==2);
    unsigned fixed=0;f->voices.updateActiveVoices(0,[](void* n,voice::Voice& v){if(v.velocity()==20)++*static_cast<unsigned*>(n);return true;},&fixed);
    APP_REQUIRE(fixed==1);
    // Sustain and controls are dispatched through the actual scheduler/router.
    f->push(E::ControlChange,64,127);f->push(E::NoteOff,60,0,8);f->push(E::ControlChange,7,90,16);
    auto pitch=f->event(E::PitchBend,0,0,24);pitch.value=10001;APP_REQUIRE(f->queue.tryPush(pitch));f->render();
    f->push(E::ControlChange,64,0);f->render();f->push(E::ControlChange,120,0);f->render();APP_REQUIRE(f->voices.activeVoiceCount()==0);
    f->push(E::NoteOn,60,100);f->render();const auto* old=f->directory.published(0);APP_REQUIRE(old);
    f->request(1);APP_REQUIRE(f->runtime.serviceChannel(0,f->b.w).ok());
    APP_REQUIRE(!f->runtime.acknowledgeCommitted(0).ok());
    f->push(E::NoteOn,60,100);f->render();++f->commits; // commit BEFORE timestamp-zero new NoteOn
    APP_REQUIRE(f->playback.active(0)==&f->b.w.snapshot && f->loops()==2);
    auto ack=f->runtime.acknowledgeCommitted(0);APP_REQUIRE(ack.ok() && ack.retired_playback==&f->a.w.snapshot);
    APP_REQUIRE(!f->directory.published(0) && !f->directory.canReclaim(old));
    APP_REQUIRE(f->residency.residentView(0).pin_count==0);
    f->render();APP_REQUIRE(std::any_of(f->pcm.begin(),f->pcm.end(),[](auto n){return n!=0;}));
    f->push(E::ControlChange,120,0);f->render();APP_REQUIRE(f->directory.canReclaim(old) && f->residency.evict(0));
    f->provider.decline=true;f->request(0);APP_REQUIRE(f->runtime.serviceChannel(0,f->a.w).ok());
    f->push(E::NoteOn,60,100);f->render();++f->commits;
    APP_REQUIRE(f->runtime.acknowledgeCommitted(0).ok() && f->loops()==0 && f->voices.activeVoiceCount()==1);
    f->push(E::ControlChange,120,0);f->render();
    f->provider.decline=false;f->provider.wrong=true;f->request(1);APP_REQUIRE(f->runtime.serviceChannel(0,f->b.w).ok());
    APP_REQUIRE(f->channel->stageReport().proposal_rejected==1);
    f->push(E::NoteOn,60,100);f->render();++f->commits;
    APP_REQUIRE(f->runtime.acknowledgeCommitted(0).ok() && f->loops()==0);f->push(E::ControlChange,120,0);f->render();
    f->provider.wrong=false;f->request(127);
    APP_REQUIRE(f->runtime.serviceChannel(0,f->a.w).status==gm::ProgramChangeServiceStatus::PresetUnavailable);
    APP_REQUIRE(f->playback.active(0)==&f->b.w.snapshot && f->channel->phase()==app::LoopProgramChannel<2>::Phase::Idle);
    APP_REQUIRE(f->renderer.stats().events_rejected==0);
    const auto frames=f->frame, hash=f->hash, events=f->renderer.stats().events_applied;
    const auto commits=f->commits;
    f->close();f->~Fixture();
    std::printf("APP_TARGET_TRACE frames=%" PRIu64 " hash=%" PRIu64 " events=%" PRIu64 " commits=%u\n",frames,hash,events,commits);

    // Real two-thread/two-task interleaving, then repeated actual MIDI switches.
    f=new(storage.data()) Fixture;f->begin(hash_fn,2);f->provider.yield=worker.yield;
    f->provider.pause.store(true);f->request(1);
    struct Pause { Fixture* f;bool success=false; } pause{f};
    worker.start([](void* p){auto& s=*static_cast<Pause*>(p);s.success=s.f->runtime.serviceChannel(0,s.f->b.w).ok();},&pause);
    while(!f->provider.entered.load(std::memory_order_acquire))worker.yield();
    APP_REQUIRE(f->samples.ready(0) && f->playback.ready(0));
    for(unsigned i=0;i<500;++i){APP_REQUIRE(f->runtime.commitReadyAtBlockBoundary()==0);if(i%32==0)worker.yield();}
    APP_REQUIRE(f->playback.active(0)==&f->a.w.snapshot);
    APP_REQUIRE(f->router.apply(f->event(E::NoteOn,60,100),f->voices));APP_REQUIRE(f->loops()==1);
    f->provider.resume.store(true,std::memory_order_release);worker.join();APP_REQUIRE(pause.success);
    APP_REQUIRE(f->runtime.commitReadyAtBlockBoundary()==1);APP_REQUIRE(f->runtime.acknowledgeCommitted(0).ok());
    f->voices.allSoundOff();f->provider.pause.store(false);
    struct Cycles { Fixture* f;Yield yield;std::atomic<unsigned> committed{0};std::atomic<bool> done{false}; } cycles{f,worker.yield};
    worker.start([](void* p){auto& s=*static_cast<Cycles*>(p);
        while(!s.done.load(std::memory_order_acquire)) {
            if(s.f->runtime.commitReadyAtBlockBoundary()) {
                APP_REQUIRE(s.f->router.apply(s.f->event(E::NoteOn,60,100),s.f->voices));
                s.f->pcm.fill(0);s.f->renderer.render(s.f->frame,s.f->pcm.data());s.f->frame+=64;
                s.f->voices.allSoundOff();s.committed.fetch_add(1,std::memory_order_release);
            }s.yield();
        }},&cycles);
    // Control-only mailbox submission here; actual MIDI PC routing is above.
    for(unsigned i=0;i<32;++i) {
        const auto program=static_cast<std::uint8_t>(i%2?1:0);auto& w=i%2?f->b:f->a;
        APP_REQUIRE(f->mailbox.submit({0,0,program,1000+i}));APP_REQUIRE(f->runtime.serviceChannel(0,w.w).ok());
        gm::ProgramChangeRetireAck result;
        do{result=f->runtime.acknowledgeCommitted(0);worker.yield();}while(!result.ok());
        APP_REQUIRE(result.token==1000+i);
        while(cycles.committed.load(std::memory_order_acquire)<i+1)worker.yield();
    }
    cycles.done.store(true,std::memory_order_release);worker.join();APP_REQUIRE(cycles.committed==32);
    f->close();f->~Fixture();
    std::puts("APP_TARGET_CONCURRENT polls=500 cycles=32 PASS");
    std::printf("APP_TARGET_LAYOUT pointer=%u fixture=%u workspace=%u channel=%u runtime=%u\n",
        unsigned(sizeof(void*)),unsigned(sizeof(Fixture)),unsigned(sizeof(Workspace)),
        unsigned(sizeof(app::LoopProgramChannel<2>)),unsigned(sizeof(app::LoopProgramRuntime<2>)));
    std::printf("APP_TARGET_RESULT suites=3 sha256=%s PASS runtime_approved=0\n",loop_app_fixture::digest_hex);
    std::fflush(stdout);
}
#undef APP_REQUIRE
} // namespace loop_app_target
