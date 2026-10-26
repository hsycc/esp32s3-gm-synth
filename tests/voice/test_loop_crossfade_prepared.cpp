#include "voice/experimental/PreparedLoopHandoff.h"
#include "sample/ProgramPrepareController.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>

using namespace gm_synth;
using namespace gm_synth::voice::experimental;
using S = PreparedLoopStatus;
using B = LoopBindingStatus;

struct Source final : sample::SampleSource {
    sample::SampleDescriptor d{};
    std::array<std::int16_t, 640> pcm{};
    unsigned calls = 0;
    Source() {
        d.id=23; d.frames=pcm.size(); d.pcm_bytes=pcm.size()*2;
        d.sample_rate=44100; d.loop_start=19; d.loop_end=595; d.flags=1;
        for (std::size_t i=0;i<pcm.size();++i) pcm[i]=static_cast<std::int16_t>(static_cast<int>((i*711)%60001)-30000);
    }
    bool descriptorFor(sample::SampleId id, sample::SampleDescriptor* out) override {
        ++calls; if (id!=d.id || !out) return false; *out=d; return true;
    }
    bool fillSample(sample::SampleId id, std::uint8_t* out, std::size_t bytes) override {
        ++calls; if (id!=d.id || bytes!=sizeof(pcm) || !out) return false;
        std::memcpy(out,pcm.data(),bytes); return true;
    }
};
struct Fixture {
    Source source;
    std::array<std::uint32_t,1024> arena{};
    sample::SampleResidencyManager residency;
    sample::PublishedSampleDirectory directory;
    sample::ProgramSnapshotExchange samples;
    sample::ProgramPrepareController controller{residency,directory,samples,&source};
    std::array<gm::PlaybackZone,3> zones{};
    gm::PlaybackProgramSnapshot snapshot{};
    PreparedLoopAsset asset{};
    PreparedLoopContextBuilder builder;
    Fixture() {
        assert(residency.begin(arena.data(),sizeof(arena)));
        const sample::SampleId id=23;
        assert(controller.activateInitialProgram(0,0,1,1,&id,1));
        for (unsigned i=0;i<zones.size();++i) {
            zones[i].sample_id=id; zones[i].loop_mode=voice::LoopMode::UntilRelease;
            zones[i].sample_region.enabled=true;
            zones[i].sample_region.start_frame=11+i;
            zones[i].sample_region.end_frame=630;
            zones[i].sample_region.loop_start_frame=23+i;
            zones[i].sample_region.loop_end_frame=610;
        }
        snapshot.channel=0; snapshot.bank=0; snapshot.program=1;
        snapshot.token=1; snapshot.bank_generation=directory.bankGeneration();
        snapshot.zones=zones.data(); snapshot.zone_count=zones.size();
        asset.sha256[0]=0x73; asset.session=11; asset.bank_generation=directory.bankGeneration();
        assert(rebind()==S::Ok);
    }
    S rebind() { return builder.begin(source,residency,directory,snapshot,*samples.active(0),asset); }
    LoopPlanRequest request(unsigned zone=0,unsigned fade=32) {
        LoopPlanRequest p;
        assert(builder.derive(static_cast<std::uint16_t>(zone),p.context)==S::Ok);
        p.experimental_opt_in=true; p.fade={fade!=0,static_cast<std::uint16_t>(fade)};
        p.release={fade!=0,static_cast<std::uint16_t>(fade?32:0)};
        return p;
    }
};

void identity() {
    Fixture f;
    LoopPlanContext c;
    assert(f.builder.derive(0,c)==S::Ok);
    assert(c.region.start==11 && c.region.loop_start==23 && c.region.loop_end==610);
    assert(c.sample_generation==f.residency.residentView(23).generation);
    assert(c.sample_rate==44100 && c.sample_type==1 && c.noteon_addresses_fixed);
    auto old=c;
    assert(f.builder.derive(99,c)==S::InvalidZone && sameContext(c,old));
    f.source.d.sample_rate=48000;
    assert(f.builder.derive(0,c)==S::SourceMismatch && sameContext(c,old));
    f.source.d.sample_rate=44100;
    f.source.d.flags=2;
    assert(f.builder.derive(0,c)==S::UnsupportedSample);
    f.source.d.flags=1;
    assert(f.residency.unpin(23));
    assert(f.builder.derive(0,c)==S::NotPinned);
    assert(f.residency.pin(23));
    auto wrong=*f.samples.active(0); wrong.token=2;
    assert(f.builder.begin(f.source,f.residency,f.directory,f.snapshot,wrong,f.asset)==S::ProgramMismatch);
    assert(!f.builder.valid());
    assert(f.rebind()==S::Ok);
    wrong=*f.samples.active(0); wrong.dependency_count=0;
    assert(f.builder.begin(f.source,f.residency,f.directory,f.snapshot,wrong,f.asset)==S::Ok);
    assert(f.builder.derive(0,c)==S::MissingDependency);
    assert(f.rebind()==S::Ok);
    f.zones[0].prepared_note_on_modulators.count=1;
    f.zones[0].prepared_note_on_modulators.routes[0]={2,0,100,0};
    assert(f.rebind()==S::Ok);
    assert(f.builder.derive(0,c)==S::DynamicAddress);
    f.zones[0].prepared_note_on_modulators.count=0;
    f.asset.sha256={}; assert(f.rebind()==S::InvalidAsset);
    assert(f.directory.stats().live_voice_refs==0);
}

void transactions() {
    Fixture f; PreparedLoopHandoff<2> h(f.directory); PinnedLoopPlayback v;
    auto p=f.request();
    assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v)==B::MissingPlan);
    assert(h.controlBegin(f.builder)==S::Ok);
    auto wrong=p; wrong.context.region.loop_start++;
    assert(h.controlAdd(f.builder,0,wrong)==S::ProposalRejected);
    wrong=p; wrong.experimental_opt_in=false;
    assert(h.controlAdd(f.builder,0,wrong)==S::ProposalRejected);
    assert(f.directory.stats().live_voice_refs==0);
    assert(h.controlAdd(f.builder,0,p)==S::Ok);
    assert(h.controlAdd(f.builder,0,p)==S::DuplicateZone);
    assert(h.controlAdd(f.builder,1,f.request(1))==S::Ok);
    assert(h.controlAdd(f.builder,2,f.request(2))==S::Full);
    assert(h.controlPublish()==S::Ok);
    assert(h.controlBegin(f.builder)==S::Busy);
    assert(h.controlAcknowledge()==S::NoAcknowledgement);
    assert(h.audioCommitAtBlockBoundary(&f.snapshot,f.samples.active(0))==S::Ok);
    assert(h.controlAcknowledge()==S::Ok);
    const auto source_calls=f.source.calls;
    assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v)==B::Ok);
    assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v)==B::Busy);
    std::int16_t x=0; assert(v.next(x)); assert(v.reset());
    assert(h.audioStart(2,&f.snapshot,f.samples.active(0),v)==B::MissingPlan);
    auto clone=f.snapshot;
    assert(h.audioStart(0,&clone,f.samples.active(0),v)==B::ContextMismatch);
    assert(f.source.calls==source_calls); // zero source access on audio APIs
    assert(h.controlBegin(f.builder)==S::Ok);
    assert(h.controlAdd(f.builder,0,p)==S::Ok);
    assert(h.controlCancel()==S::Ok);
    assert(h.controlBegin(f.builder)==S::Ok); // mismatched snapshot commit is rejected
    assert(h.controlPublish()==S::Ok);
    assert(h.audioCommitAtBlockBoundary(&clone,f.samples.active(0))==S::CommitRejected);
    assert(h.controlAcknowledge()==S::Ok);
    assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v)==B::Ok); // old active survives
    assert(v.reset()); assert(h.resetQuiescent());
    assert(f.directory.stats().live_voice_refs==0);
}

void real_retirement() {
    Fixture f; PreparedLoopHandoff<> h(f.directory); PinnedLoopPlayback v;
    assert(h.controlBegin(f.builder)==S::Ok);
    assert(h.controlAdd(f.builder,0,f.request())==S::Ok);
    assert(h.controlPublish()==S::Ok);
    assert(h.audioCommitAtBlockBoundary(&f.snapshot,f.samples.active(0))==S::Ok);
    assert(h.controlAcknowledge()==S::Ok);
    assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v)==B::Ok);
    const auto* old_record=f.directory.published(23);
    // Retire through the PRODUCTION coordinator, not a mocked refcount.
    assert(f.controller.prepareProgram(0,0,2,2,nullptr,0));
    gm::PlaybackProgramSnapshot empty{};
    empty.channel=0; empty.program=2; empty.token=2; empty.bank_generation=1;
    PreparedLoopContextBuilder next;
    assert(next.begin(f.source,f.residency,f.directory,empty,*f.samples.ready(0),f.asset)==S::Ok);
    assert(h.controlBegin(next)==S::Ok); assert(h.controlPublish()==S::Ok);
    assert(f.samples.commitAtBlockBoundary(0));
    assert(h.audioCommitAtBlockBoundary(&empty,f.samples.active(0))==S::Ok);
    assert(h.controlAcknowledge()==S::Ok);
    assert(f.controller.acknowledgeAndRetire(0,2).valid());
    assert(f.directory.published(23)==nullptr);
    assert(!f.directory.canReclaim(old_record));
    assert(f.residency.residentView(23).pin_count==0);
    std::array<std::int16_t,598> pre{};
    assert(v.render(pre.data(),pre.size())==pre.size());
    v.noteOff(); std::array<std::int16_t,128> tail{};
    assert(v.render(tail.data(),tail.size())>0 && !v.active());
    assert(f.directory.canReclaim(old_record));
    assert(f.residency.evict(23));
    std::int16_t sentinel=9; assert(!v.next(sentinel) && sentinel==9);
    assert(h.resetQuiescent());
    assert(f.directory.stats().live_voice_refs==0 && f.directory.stats().stale_release_faults==0);
}

void pcm_matrix() {
    unsigned count=0;
    for (unsigned fade : {0u,16u,32u,64u}) for (auto step : {LoopCrossfadeCursor::kOne/2,
         LoopCrossfadeCursor::kOne,5*LoopCrossfadeCursor::kOne/4}) for (unsigned block : {1u,17u,64u}) {
        Fixture f; PreparedLoopHandoff<> h(f.directory); PinnedLoopPlayback v;
        auto p=f.request(0,fade);
        assert(h.controlBegin(f.builder)==S::Ok && h.controlAdd(f.builder,0,p)==S::Ok);
        assert(h.controlPublish()==S::Ok);
        assert(h.audioCommitAtBlockBoundary(&f.snapshot,f.samples.active(0))==S::Ok);
        assert(h.controlAcknowledge()==S::Ok);
        assert(h.audioStart(0,&f.snapshot,f.samples.active(0),v,step)==B::Ok);
        BoundedLoopReleaseCursor oracle;
        assert(oracle.start(f.source.pcm.data(),f.source.pcm.size(),p.context.region,p.context.mode,step,p.fade,p.release));
        assert(h.resetQuiescent()); // playback owns its copy and pin
        std::array<std::int16_t,64> a{},b{};
        for (unsigned i=0;i<1400;) {
            if (i==1100) { v.noteOff(); oracle.noteOff(); }
            unsigned n=std::min(block,1400-i);
            if (i<1100) n=std::min(n,1100-i);
            const auto x=v.render(a.data(),n), y=oracle.render(b.data(),n);
            assert(x==y);
            for (std::size_t j=0;j<x;++j) assert(a[j]==b[j]);
            i+=n;
        }
        assert(v.reset()); ++count;
    }
    std::cout<<"Prepared PCM matrix: "<<count<<" exact cases PASS\n";
}

void concurrent_handoff() {
    Fixture f; PreparedLoopHandoff<2> h(f.directory);
    const auto proposal=f.request();
    constexpr unsigned rounds=2000;
    std::atomic<unsigned> committed{0};
    std::thread audio([&] {
        for (unsigned i=0;i<rounds;++i) {
            while (h.audioCommitAtBlockBoundary(&f.snapshot,f.samples.active(0))==S::NoReady)
                std::this_thread::yield();
            PinnedLoopPlayback voice;
            assert(h.audioStart(0,&f.snapshot,f.samples.active(0),voice)==B::Ok);
            std::int16_t out=0; assert(voice.next(out));
            committed.fetch_add(1,std::memory_order_release);
        }
    });
    for (unsigned i=0;i<rounds;++i) {
        assert(h.controlBegin(f.builder)==S::Ok);
        assert(h.controlAdd(f.builder,0,proposal)==S::Ok);
        assert(h.controlPublish()==S::Ok);
        while (h.controlAcknowledge()==S::NoAcknowledgement) std::this_thread::yield();
        // No wait for playback: the next plan may retire while that voice runs.
    }
    audio.join(); assert(committed.load()==rounds);
    assert(h.resetQuiescent());
    assert(f.directory.stats().live_voice_refs==0 && f.directory.stats().stale_release_faults==0);
    std::cout<<"SPSC prepare/commit/retire: "<<rounds<<" concurrent replacements PASS\n";
}

int main() {
    identity(); transactions(); real_retirement(); pcm_matrix(); concurrent_handoff();
    std::cout<<"Prepared context/handoff: 5 suites PASS; no production opt-in\n";
}
