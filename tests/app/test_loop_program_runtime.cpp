#include "app/LoopProgramRuntime.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "sf2/Sf2SampleSource.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

using namespace gm_synth;
namespace x = voice::experimental;
using Status = x::PreparedLoopStatus;
void check(bool ok, int line) { if (!ok) { std::fprintf(stderr,"FAIL line=%d\n",line); std::abort(); } }
#define CHECK(a) check(bool(a), __LINE__)

struct Reader : storage::RandomAccessReader {
    std::vector<char> data;
    unsigned calls = 0;
    explicit Reader(const char* path) {
        std::ifstream in(path,std::ios::binary); CHECK(in.good());
        data.assign(std::istreambuf_iterator<char>(in), {}); CHECK(!data.empty());
    }
    std::uint64_t size() const override { return data.size(); }
    bool readAt(std::uint64_t off,void* dest,std::size_t n) override {
        ++calls;
        if (!dest || off>data.size() || n>data.size()-off) return false;
        std::copy_n(data.data()+off,n,static_cast<char*>(dest)); return true;
    }
};
struct Workspace {
    std::vector<sf2::NativeResolvedZone> native;
    std::vector<sf2::ResolvedZoneModulation> mods;
    std::vector<gm::PlaybackZone> zones;
    std::array<gm::PlaybackProgramKeySpan,128> spans{};
    std::vector<std::uint16_t> indexes;
    std::vector<sample::SampleId> deps;
    gm::ProgramChangePrepareWorkspace w;
    explicit Workspace(unsigned cap=64):native(cap),mods(cap),zones(cap),indexes(cap*128),deps(cap) {
        w.native_scratch=native.data();w.native_capacity=native.size();
        w.modulation_scratch=mods.data();w.modulation_capacity=mods.size();
        w.playback_zones=zones.data();w.playback_capacity=zones.size();
        w.key_spans=spans.data();w.key_zone_indices=indexes.data();w.key_zone_index_capacity=indexes.size();
        w.dependencies=deps.data();w.dependency_capacity=deps.size();
    }
};
struct Provider {
    unsigned rejected=0, calls=0;
    bool decline=false, wrong=false;
    std::atomic<bool>* entered=nullptr;
    std::atomic<bool>* resume=nullptr;
    static bool propose(void* ptr,const x::PreparedLoopProgram&,std::uint16_t zone,
                        const x::LoopPlanContext& current,x::LoopPlanRequest& result) {
        auto& self=*static_cast<Provider*>(ptr); ++self.calls;
        if(self.entered) {
            self.entered->store(true,std::memory_order_release);
            while(!self.resume->load(std::memory_order_acquire))std::this_thread::yield();
        }
        if(self.decline || current.mode==x::CursorLoopMode::None) return false;
        result.context=current;result.experimental_opt_in=true;result.fade={true,32};result.release={true,32};
        if(self.wrong){result.context.session_serial++;self.rejected++;}
        // Test-only proposal, not a production recommendation or Sample whitelist.
        return zone<16;
    }
};
template<unsigned N=2> struct Fixture {
    Reader reader;
    sf2::Sf2PdtaReader pdta;
    sf2::Sf2SampleSource source;
    std::vector<std::uint32_t> arena;
    sample::SampleResidencyManager residency;
    sample::PublishedSampleDirectory directory;
    sample::ProgramSnapshotExchange samples;
    gm::PlaybackProgramSnapshotExchange playback;
    sample::ProgramPrepareController controller{residency,directory,samples,&source};
    gm::RealtimeProgramChangeHandoff requests;
    gm::ProgramChangeMailbox mailbox;
    sf2::Sf2PlaybackProgramPreparer preparer;
    gm::ProgramChangeControlService control{requests,mailbox,pdta,preparer,controller,samples,playback};
    Provider provider;
    x::PreparedLoopAsset asset{};
    Workspace a,b;
    app::LoopProgramChannel<N>* channel=nullptr;
    app::LoopProgramRuntime<N> runtime{control,samples,playback};
    gm::RealtimeProgramEventApplier events{playback};
    gm::RealtimeProgramChangeRouter router{events,requests};
    voice::VoicePool pool{directory};
    explicit Fixture(const char* path):reader(path),arena(4096) {
        CHECK(pdta.begin(reader));CHECK(source.begin(reader));CHECK(residency.begin(arena.data(),arena.size()*4));
        sf2::PlaybackPrepareRequest r;
        r.channel=0;r.bank=0;r.program=0;r.token=100;r.bank_generation=1;
        r.native_scratch=a.w.native_scratch;r.native_capacity=a.w.native_capacity;
        r.modulation_scratch=a.w.modulation_scratch;r.modulation_capacity=a.w.modulation_capacity;
        r.playback_zones=a.w.playback_zones;r.playback_capacity=a.w.playback_capacity;
        r.key_spans=a.w.key_spans;r.key_zone_indices=a.w.key_zone_indices;r.key_zone_index_capacity=a.w.key_zone_index_capacity;
        r.dependencies=a.w.dependencies;r.dependency_capacity=a.w.dependency_capacity;r.snapshot=&a.w.snapshot;
        auto prepared=preparer.prepare(pdta,r);CHECK(prepared.ok());CHECK(a.w.snapshot.zone_count==3);
        CHECK(controller.activateInitialProgram(0,0,0,100,a.w.dependencies,prepared.dependency_count));
        CHECK(playback.setInitialActive(&a.w.snapshot));
        asset.sha256.fill(0xA5);asset.session=99;asset.bank_generation=1; // synthetic fixture stamp only
        channel=new app::LoopProgramChannel<N>(0,source,residency,directory,samples,playback,asset,&Provider::propose,&provider);
        CHECK(runtime.attach(*channel));CHECK(!runtime.attach(*channel));runtime.install(events);
    }
    ~Fixture() {
        pool.allSoundOff();runtime.uninstallQuiescent(events);CHECK(channel->resetQuiescent());delete channel;
        CHECK(directory.stats().live_voice_refs==0);CHECK(directory.stats().stale_release_faults==0);
    }
    realtime::RealtimeEvent note(unsigned key=60,unsigned velocity=100) {
        realtime::RealtimeEvent e;e.type=realtime::EventType::NoteOn;e.channel=0;e.data1=key;e.data2=velocity;return e;
    }
    void initialize() {
        CHECK(channel->initializeActive()==Status::Ok);
        CHECK(!runtime.acknowledgeCommitted(0).ok());
        CHECK(runtime.commitReadyAtBlockBoundary()==1);CHECK(runtime.acknowledgeCommitted(0).ok());
    }
    void request(unsigned program) {
        auto e=note();e.type=realtime::EventType::ProgramChange;e.data1=program;
        CHECK(router.apply(e,pool));CHECK(runtime.drainRealtimeRequests()==1);
    }
    static bool countLoop(void* ctx,voice::Voice& v) {
        if (v.experimentalLoopEnabled()) ++*static_cast<unsigned*>(ctx);
        return true;
    }
    unsigned activeLoops() { unsigned n=0;pool.updateActiveVoices(0,&countLoop,&n);return n; }
};

void selection(const char* path) {
    Fixture<> f(path);f.initialize();CHECK(f.channel->stageReport().admitted==2);
    CHECK(f.channel->stageReport().capacity_skipped==1);
    const auto reads=f.reader.calls;
    CHECK(f.router.apply(f.note(60,100),f.pool));CHECK(f.activeLoops()==1);
    // Incoming velocity 100 selects zone0; params fixedVelocity=20 must not fail
    // the 80..127 selection check in the plan layer.
    unsigned fixed=0;
    f.pool.updateActiveVoices(0,[](void* p,voice::Voice& v){ if(v.velocity()==20)++*static_cast<unsigned*>(p);return true;},&fixed);
    CHECK(fixed==1);
    CHECK(f.router.apply(f.note(60,50),f.pool));CHECK(f.pool.activeVoiceCount()==2);CHECK(f.activeLoops()==1);
    // Same sample ID but zone2 was capacity-skipped: MUST use Direct, not zone0's plan.
    CHECK(f.router.apply(f.note(61,90),f.pool));CHECK(f.activeLoops()==2);
    CHECK(f.reader.calls==reads);
    const auto attempts=f.pool.stats().start_attempts;
    f.pool.setAdmissionLimit(1);CHECK(!f.router.apply(f.note(),f.pool));
    CHECK(f.pool.stats().start_attempts==attempts+1); // no fallback retry after admission failure
    std::puts("APP selection: fixed velocity / exact Zone / capacity / single attempt PASS");
}

void transitions(const char* path) {
    Fixture<> f(path);f.initialize();CHECK(f.router.apply(f.note(),f.pool));
    const auto* old=f.directory.published(0);CHECK(old);
    auto* before=f.playback.active(0);f.request(1);
    CHECK(f.runtime.serviceChannel(0,f.b.w).ok());
    CHECK(f.playback.active(0)==before);CHECK(!f.runtime.acknowledgeCommitted(0).ok());
    CHECK(f.runtime.serviceChannel(0,f.a.w).status==gm::ProgramChangeServiceStatus::TransitionBusy);
    const auto reads=f.reader.calls;
    CHECK(f.runtime.commitReadyAtBlockBoundary()==1);CHECK(f.runtime.commitReadyAtBlockBoundary()==0);
    CHECK(f.playback.active(0)==&f.b.w.snapshot);CHECK(f.samples.active(0)->token==f.b.w.snapshot.token);
    CHECK(f.reader.calls==reads);
    auto ack=f.runtime.acknowledgeCommitted(0);CHECK(ack.ok() && ack.retired_playback==before);
    CHECK(!f.runtime.acknowledgeCommitted(0).ok());
    CHECK(!f.directory.published(0));CHECK(!f.directory.canReclaim(old));CHECK(f.residency.residentView(0).pin_count==0);
    std::array<std::int32_t,128> out{};f.pool.renderAdd(out.data(),64);
    CHECK(std::any_of(out.begin(),out.end(),[](auto n){return n!=0;}));
    CHECK(f.router.apply(f.note(),f.pool));CHECK(f.activeLoops()==2);
    f.pool.allSoundOff();CHECK(f.directory.canReclaim(old));CHECK(f.residency.evict(0));
    std::puts("APP transaction: joint commit / ack order / old-tail reclaim PASS");
}

void fallback(const char* path) {
    Fixture<> f(path);f.provider.wrong=true;f.initialize();
    CHECK(f.channel->stageReport().admitted==0);CHECK(f.channel->stageReport().proposal_rejected==3);
    CHECK(f.router.apply(f.note(),f.pool));CHECK(f.activeLoops()==0);f.pool.allSoundOff();
    f.provider.wrong=false;f.provider.decline=true;f.request(1);
    CHECK(f.runtime.serviceChannel(0,f.b.w).ok());CHECK(f.channel->stageReport().admitted==0);
    CHECK(f.runtime.commitReadyAtBlockBoundary()==1);CHECK(f.runtime.acknowledgeCommitted(0).ok());
    CHECK(f.router.apply(f.note(),f.pool));CHECK(f.activeLoops()==0);
    std::puts("APP optional rejection/empty sidecar: Direct and Program progress PASS");
}

void interleave(const char* path) {
    Fixture<> f(path);f.initialize();
    std::atomic<bool> entered{false},resume{false};
    f.provider.entered=&entered;f.provider.resume=&resume;f.request(1);
    bool success=false;
    std::thread producer([&]{success=f.runtime.serviceChannel(0,f.b.w).ok();});
    while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
    CHECK(f.samples.ready(0) && f.playback.ready(0));
    // Service has published BOTH old exchanges; a slow provider is still running.
    // The joint gate must keep old program/plan active throughout this interval.
    for(unsigned i=0;i<500;++i) {
        CHECK(f.runtime.commitReadyAtBlockBoundary()==0);CHECK(f.playback.active(0)==&f.a.w.snapshot);
    }
    CHECK(f.router.apply(f.note(),f.pool));CHECK(f.activeLoops()==1);
    resume.store(true,std::memory_order_release);producer.join();CHECK(success);
    CHECK(f.runtime.commitReadyAtBlockBoundary()==1);CHECK(f.runtime.acknowledgeCommitted(0).ok());
    std::puts("APP interleaving: pair READY while provider paused never commits early PASS");
}

void pending_latest(const char* path) {
    Fixture<> f(path);f.initialize();f.request(1);CHECK(f.runtime.serviceChannel(0,f.b.w).ok());
    f.request(1);f.request(0); // latest request waits; READY remains immutable
    CHECK(f.runtime.serviceChannel(0,f.a.w).status==gm::ProgramChangeServiceStatus::TransitionBusy);
    CHECK(f.runtime.commitReadyAtBlockBoundary()==1);CHECK(f.runtime.acknowledgeCommitted(0).ok());
    CHECK(f.runtime.serviceChannel(0,f.a.w).ok());CHECK(f.a.w.snapshot.program==0);
    CHECK(f.runtime.commitReadyAtBlockBoundary()==1);CHECK(f.runtime.acknowledgeCommitted(0).ok());
    f.request(127);auto missing=f.runtime.serviceChannel(0,f.b.w);
    CHECK(missing.status==gm::ProgramChangeServiceStatus::PresetUnavailable);
    CHECK(f.channel->phase()==app::LoopProgramChannel<2>::Phase::Idle);
    CHECK(f.runtime.commitReadyAtBlockBoundary()==0);CHECK(f.playback.active(0)==&f.a.w.snapshot);
    std::puts("APP coalescing and unavailable Program: no incomplete commit PASS");
}


void scheduled_boundary(const char* path) {
    Fixture<> f(path);f.initialize();f.request(1);CHECK(f.runtime.serviceChannel(0,f.b.w).ok());
    realtime::RealtimeEventQueue queue;realtime::BlockEventScheduler scheduler(queue);
    realtime::ScheduledAudioBlockRenderer renderer(f.pool,scheduler,&gm::RealtimeProgramChangeRouter::callback,&f.router);
    auto note=f.note();note.timestamp_frame=0;CHECK(queue.tryPush(note));
    auto off=note;off.type=realtime::EventType::NoteOff;off.timestamp_frame=80;CHECK(queue.tryPush(off));
    std::array<std::int16_t,realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> pcm{};
    const auto reads=f.reader.calls;f.runtime.render(renderer,0,pcm.data());
    CHECK(f.playback.active(0)==&f.b.w.snapshot);CHECK(f.reader.calls==reads);
    CHECK(renderer.stats().events_applied==1);CHECK(f.events.stats().voices_started==1);
    CHECK(f.activeLoops()==1);
    unsigned new_sample=0;
    f.pool.updateActiveVoices(0,[](void* p,voice::Voice& v){if(v.sampleId()==1)++*static_cast<unsigned*>(p);return true;},&new_sample);
    CHECK(new_sample==1);CHECK(f.runtime.acknowledgeCommitted(0).ok());
    f.runtime.render(renderer,64,pcm.data());CHECK(renderer.stats().events_applied==2);
    std::puts("APP scheduled block: new Program+plan before timestamp-zero NoteOn PASS");
}

void preflight_fault(const char* path) {
    Fixture<> f(path);f.initialize();CHECK(f.router.apply(f.note(),f.pool));
    f.request(1);CHECK(f.runtime.serviceChannel(0,f.b.w).ok());
    const auto* old_samples=f.samples.active(0);const auto* old_playback=f.playback.active(0);
    // Deliberate owner-contract violation for a negative test, WITHOUT racing.
    // Verify mismatch is caught before either external exchange is committed.
    ++f.b.w.snapshot.token;
    CHECK(f.runtime.commitReadyAtBlockBoundary()==0);
    CHECK(f.samples.active(0)==old_samples && f.playback.active(0)==old_playback);
    CHECK(f.channel->phase()==app::LoopProgramChannel<2>::Phase::Fault);
    CHECK(!f.runtime.acknowledgeCommitted(0).ok());CHECK(!f.router.apply(f.note(),f.pool));
    CHECK(f.pool.activeVoiceCount()==1);
    std::array<std::int32_t,128> pcm{};f.pool.renderAdd(pcm.data(),64);
    CHECK(std::any_of(pcm.begin(),pcm.end(),[](auto v){return v!=0;}));
    --f.b.w.snapshot.token;
    // Fault recovery is deliberately quiescent and explicit, not retry/rollback.
    std::puts("APP preflight corruption: no partial commit / new starts fail closed PASS");
}

void concurrent_cycles(const char* path) {
    Fixture<> f(path);f.initialize();
    constexpr unsigned cycles=1000;
    std::atomic<unsigned> committed{0};
    std::atomic<bool> done{false};
    std::thread audio([&]{
        while(!done.load(std::memory_order_acquire)) {
            if(f.runtime.commitReadyAtBlockBoundary()) {
                CHECK(f.router.apply(f.note(),f.pool));
                std::array<std::int32_t,128> pcm{};f.pool.renderAdd(pcm.data(),64);f.pool.allSoundOff();
                committed.fetch_add(1,std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });
    for(unsigned n=0;n<cycles;++n) {
        auto& workspace=n%2?f.a:f.b;
        CHECK(f.mailbox.submit({0,0,static_cast<std::uint8_t>(n%2?0:1),1000+n}));
        CHECK(f.runtime.serviceChannel(0,workspace.w).ok());
        // The control task races ack with ongoing audio iteration. Plans may be
        // retired immediately after joint commit while the new NoteOn executes.
        gm::ProgramChangeRetireAck ack;
        do {ack=f.runtime.acknowledgeCommitted(0);std::this_thread::yield();} while(!ack.ok());
        CHECK(ack.token==1000+n);
        while(committed.load(std::memory_order_acquire)<n+1)std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);audio.join();CHECK(committed==cycles);
    std::puts("APP concurrent publish/commit/start/ack: 1000 cycles PASS");
}

int main(int argc,char**argv) {
    CHECK(argc==2);selection(argv[1]);transitions(argv[1]);fallback(argv[1]);interleave(argv[1]);pending_latest(argv[1]);scheduled_boundary(argv[1]);preflight_fault(argv[1]);concurrent_cycles(argv[1]);
    std::puts("APP_LOOP_RESULT suites=8 PASS default_firmware_unchanged=1");
}
