#include "app/LoopStartupSession.h"
#include "loop_analysis_fixture.h"
#include <openssl/sha.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace gm_synth;
using Session=app::LoopStartupSession<>;
using Status=app::LoopStartupStatus;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)){ std::fprintf(stderr,"STARTUP_FAIL line=%d\n",__LINE__);std::abort();} } while(false)
static bool hash(const std::uint8_t* p,std::size_t n,app::VerifiedReadOnlyBank::Digest& out) noexcept {
    return SHA256(p,n,out.data())!=nullptr;
}
static bool nohash(const std::uint8_t*,std::size_t,app::VerifiedReadOnlyBank::Digest&) noexcept {return false;}
struct Backing {
    std::uint8_t* bytes=nullptr;void* arena=nullptr;unsigned releases=0;Session* observer=nullptr;
    app::StartupLease lease(std::size_t capacity=20*1024) {
        CHECK(!bytes && !arena);
        bytes=static_cast<std::uint8_t*>(std::malloc(sizeof(loop_analysis_fixture::bytes)));
        arena=std::malloc(capacity);CHECK(bytes && arena);
        std::memcpy(bytes,loop_analysis_fixture::bytes,sizeof(loop_analysis_fixture::bytes));
        return {bytes,sizeof(loop_analysis_fixture::bytes),arena,capacity,this,&release};
    }
    static void release(void* opaque) noexcept {
        auto& b=*static_cast<Backing*>(opaque);
        if(b.observer) CHECK(b.observer->activeVoices()==0 && b.observer->activeLoops()==0);
        std::free(b.bytes);std::free(b.arena);b.bytes=nullptr;b.arena=nullptr;++b.releases;
    }
    ~Backing(){CHECK(!bytes && !arena);}
};
static realtime::RealtimeEvent event(Session& s,realtime::EventType type,unsigned a,unsigned b=0) {
    realtime::RealtimeEvent e{};e.channel=0;e.type=type;e.data1=a;e.data2=b;e.timestamp_frame=s.frame();return e;
}
static std::uint64_t script(Session& s,unsigned blocks=256) {
    std::array<std::int16_t,128> out{};std::uint64_t trace=UINT64_C(14695981039346656037);
    CHECK(s.enqueue(event(s,realtime::EventType::NoteOn,60,100)));
    const auto work=s.budget().used;
    for(unsigned i=0;i<blocks;++i) {
        if(i==128) CHECK(s.enqueue(event(s,realtime::EventType::ProgramChange,1)));
        if(i==129) CHECK(s.enqueue(event(s,realtime::EventType::NoteOn,60,100)));
        CHECK(s.render(out.data()));
        if(i<128)CHECK(s.budget().used==work);
        if(i==128) CHECK(s.service()==gm::ProgramChangeServiceStatus::Ok);
        if(i==129) {CHECK(s.activeProgram()==1);CHECK(s.service()==gm::ProgramChangeServiceStatus::NoPending);}
        for(auto v:out){trace^=static_cast<std::uint16_t>(v);trace*=UINT64_C(1099511628211);}
        trace^=s.activeVoices();trace*=UINT64_C(1099511628211);
    }
    CHECK(s.audioStats().events_rejected==0);return trace;
}
int main() {
    app::StartupSerials serials;unsigned suites=0;std::uint64_t automatic=0,direct=0,last=0;
    for(bool enabled:{false,true}) {
        Backing b;auto s=std::make_unique<Session>();b.observer=s.get();
        app::LoopStartupConfig c;c.analysis.experimental_opt_in=enabled;
        CHECK(s->start(b.lease(),loop_analysis_fixture::digest,hash,serials,c)==Status::Running);
        CHECK(s->serial()>last);last=s->serial();
        CHECK((s->stage().admitted>0)==enabled);CHECK(b.releases==0);
        auto t=script(*s);if(enabled)automatic=t;else direct=t;
        CHECK(s->stop() && !s->running() && b.releases==1);
        std::array<std::int16_t,128> out{};CHECK(!s->render(out.data()));
        CHECK(!s->enqueue(event(*s,realtime::EventType::NoteOn,60,100)));
        CHECK(s->stop() && b.releases==1);s.reset();CHECK(b.releases==1);++suites;
    }
    CHECK(automatic!=direct);
    // Failure after each ownership boundary must free the adopted resource.
    for(unsigned kind=0;kind<6;++kind) {
        Backing b;auto s=std::make_unique<Session>();b.observer=s.get();
        auto d=loop_analysis_fixture::digest;app::LoopStartupConfig c;
        auto lease=b.lease(kind==3?4:20*1024);
        if(kind==0)d[0]^=1;
        if(kind==1)b.bytes[0]='X';
        if(kind==2)c.program=127;
        app::StartupSerials exhausted(UINT64_MAX);
        auto result=s->start(std::move(lease),d,kind==4?nohash:hash,kind==5?exhausted:serials,c);
        CHECK(result!=Status::Running && !s->running() && b.releases==1);
        CHECK(!lease.valid());s.reset();CHECK(b.releases==1);++suites;
    }
    // Reject rebind without touching the active bank or the current serial.
    {
        Backing a,b;auto s=std::make_unique<Session>();a.observer=s.get();
        CHECK(s->start(a.lease(),loop_analysis_fixture::digest,hash,serials)==Status::Running);
        auto id=s->serial();
        CHECK(s->start(b.lease(),loop_analysis_fixture::digest,hash,serials)==Status::AlreadyUsed);
        CHECK(a.releases==0 && b.releases==1 && s->serial()==id && s->running());
        CHECK(s->enqueue(event(*s,realtime::EventType::NoteOn,60,100)));
        std::array<std::int16_t,128> out{};CHECK(s->render(out.data()) && s->activeVoices()>0);
        s.reset();CHECK(a.releases==1);++suites;
    }
    // Stop in READY and Committed-but-unacknowledged states: whole-group stop.
    for(bool commit:{false,true}) {
        Backing b;auto s=std::make_unique<Session>();b.observer=s.get();app::LoopStartupConfig c;c.analysis.experimental_opt_in=true;
        CHECK(s->start(b.lease(),loop_analysis_fixture::digest,hash,serials,c)==Status::Running);
        std::array<std::int16_t,128> out{};
        CHECK(s->enqueue(event(*s,realtime::EventType::NoteOn,60,100)) && s->render(out.data()));
        CHECK(s->enqueue(event(*s,realtime::EventType::ProgramChange,1)) && s->render(out.data()));
        CHECK(s->service()==gm::ProgramChangeServiceStatus::Ok);
        if(commit)CHECK(s->render(out.data()));
        CHECK(s->stop() && b.releases==1);++suites;
    }
    // Direct fallbacks still load and render a valid Program.
    for(unsigned kind=0;kind<3;++kind) {
        Backing b;auto s=std::make_unique<Session>();b.observer=s.get();app::LoopStartupConfig c;c.analysis.experimental_opt_in=true;
        if(kind==0)c.analysis.max_work_per_program=0;
        if(kind==1)c.analysis.max_analyses_per_program=0;
        if(kind==2)c.analysis.checkpoint=+[](void*){return false;};
        CHECK(s->start(b.lease(),loop_analysis_fixture::digest,hash,serials,c)==Status::Running);
        CHECK(s->stage().admitted==0 && script(*s)==direct);CHECK(s->stop());++suites;
    }
    // Post-free entry points are inert under ASan. New groups get fresh serials.
    for(unsigned i=0;i<24;++i) {
        Backing b;auto s=std::make_unique<Session>();b.observer=s.get();
        CHECK(s->start(b.lease(),loop_analysis_fixture::digest,hash,serials)==Status::Running);
        CHECK(s->serial()>last);last=s->serial();CHECK(s->stop());
        CHECK(s->activeProgram()==255 && s->service()==gm::ProgramChangeServiceStatus::NoActiveResidencySnapshot);
    }
    ++suites;
    app::StartupSerials edge(UINT64_MAX-1);CHECK(edge.next()==UINT64_MAX && edge.next()==0 && edge.next()==0);
    ++suites;
    std::printf("STARTUP_HOST suites=%u checks=%u automatic=%llu direct=%llu frames=16384\nSTARTUP_HOST_PASS\n",suites,checks,
        static_cast<unsigned long long>(automatic),static_cast<unsigned long long>(direct));
}
