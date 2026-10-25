#include "voice/experimental/BoundedLoopReleaseCursor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using namespace gm_synth::voice::experimental;
using Base = LoopCrossfadeCursor;
using Bridge = BoundedLoopReleaseCursor;
constexpr auto Q = Base::kOne;

std::vector<std::int16_t> signal(unsigned n) {
    std::vector<std::int16_t> pcm(n);
    std::uint32_t seed=731;
    for (auto& x:pcm) {
        seed=1664525u*seed+1013904223u;
        x=static_cast<std::int16_t>(static_cast<int>(seed%65536)-32768);
    }
    return pcm;
}
std::int16_t mix(std::int16_t a, std::int16_t b, unsigned j, unsigned n) {
    const std::int64_t sum=std::int64_t{a}*(n-1-j)+std::int64_t{b}*j;
    const auto mag=sum<0 ? -sum : sum;
    const auto rounded=(mag+(n-1)/2)/(n-1);
    return static_cast<std::int16_t>(sum<0 ? -rounded : rounded);
}

void unchanged() {
    const auto pcm=signal(587);
    const CursorRegion r{11,573,23,523};
    unsigned count=0;
    for(auto mode:{CursorLoopMode::None,CursorLoopMode::Continuous,CursorLoopMode::UntilRelease})
    for(unsigned fade:{0u,16u,32u,64u}) {
        if(mode==CursorLoopMode::None && fade) continue;
        for(auto step:{Q/2,Q,5*Q/4,3*Q+9}) for(unsigned off:{0u,219u,501u,10000u})
        for(bool opt:{false,true}) {
            // Direct is always unchanged, as are Continuous/None and disabled bridges.
            if(opt && fade && mode==CursorLoopMode::UntilRelease) continue;
            Base a; Bridge b;
            ExperimentalFade f{fade!=0,static_cast<std::uint16_t>(fade)};
            assert(a.start(pcm.data(),pcm.size(),r,mode,step,f));
            assert(b.start(pcm.data(),pcm.size(),r,mode,step,f,{opt,32}));
            for(unsigned t=0;t<2200;++t) {
                if(t==off) { a.noteOff(); b.noteOff(); }
                std::int16_t x=123,y=123;
                assert(a.next(x)==b.next(y));
                assert(x==y && a.phaseQ32()==b.phaseQ32() && a.active()==b.active());
                assert(!b.transitionStarted());
            }
            ++count;
        }
    }
    std::cout << "Unchanged Direct/disabled/mode cases: " << count << " PASS\n";
}

void oracleMatrix() {
    const auto pcm=signal(900);
    unsigned cases=0;
    for(unsigned fade:{16u,32u,64u}) for(unsigned duration:{16u,32u,64u})
    for(auto step:{Q/2,Q,5*Q/4,3*Q+7})
    for(unsigned offset:{0u,1u,8u,31u,63u}) for(bool short_tail:{false,true}) {
        const CursorRegion r{11,short_tail?531u:900u,19,531};
        const unsigned off=static_cast<unsigned>(((531-fade+offset-11)*Q)/step);
        Base held, raw; Bridge b;
        const ExperimentalFade f{true,static_cast<std::uint16_t>(fade)};
        const auto mode=CursorLoopMode::UntilRelease;
        assert(held.start(pcm.data(),pcm.size(),r,mode,step,f));
        assert(raw.start(pcm.data(),pcm.size(),r,mode,step,f));
        assert(b.start(pcm.data(),pcm.size(),r,mode,step,f,{true,static_cast<std::uint16_t>(duration)}));
        bool started=false; unsigned rendered=0;
        for(unsigned t=0;t<3000 && raw.active();++t) {
            if(t==off) {
                const auto phase=b.phaseQ32(); raw.noteOff(); b.noteOff();
                assert(b.phaseQ32()==phase && !b.looping());
                started=b.transitionStarted();
            }
            // Exercise pitch-rate updates during the bridge and raw tail.
            if(t==off+5) {
                assert(held.setStep(step+Q/8)); assert(raw.setStep(step+Q/8)); assert(b.setStep(step+Q/8));
            }
            std::int16_t h=0,x=0,y=0;
            assert(held.next(h)); assert(raw.next(x)); assert(b.next(y));
            std::int16_t expected=x;
            if(started && rendered<duration) {
                const unsigned j=raw.active()?rendered:duration-1;
                expected=mix(h,x,j,duration); ++rendered;
                assert(y>=std::min(h,x) && y<=std::max(h,x));
                if(rendered==1 && raw.active()) assert(y==h);
                if(!raw.active() || rendered==duration) assert(y==x);
            }
            assert(expected==y && b.phaseQ32()==raw.phaseQ32());
            assert(b.active()==raw.active());
            if(t>=off) { b.noteOff(); assert(!b.looping()); } // cannot restart the bridge
        }
        assert(b.transitionFramesRendered()<=duration);
        assert(!b.transitionActive()); ++cases;
    }
    std::cout << "Release oracle/rate/tail matrix: " << cases << " PASS\n";
}

std::vector<std::int16_t> blocked(std::size_t block, unsigned off) {
    const auto pcm=signal(900);
    Bridge b;
    assert(b.start(pcm.data(),pcm.size(),{0,900,19,531},CursorLoopMode::UntilRelease,
                   5*Q/4,{true,32},{true,32}));
    std::vector<std::int16_t> out;
    std::array<std::int16_t,257> scratch{};
    unsigned t=0;
    while(b.active() && t<3000) {
        if(t==off) b.noteOff();
        auto n=std::min<std::size_t>(block,3000-t);
        if(t<off) n=std::min<std::size_t>(n,off-t);
        const auto got=b.render(scratch.data(),n);
        out.insert(out.end(),scratch.begin(),scratch.begin()+got); t+=got;
    }
    return out;
}
void blockSplits() {
    for(unsigned off:{399u,400u,411u,425u,800u}) {
        const auto ref=blocked(1,off);
        for(auto block:{17u,64u,65u,257u}) assert(ref==blocked(block,off));
    }
}
void endAndFraction() {
    auto pcm=signal(600);
    Bridge b;
    assert(b.start(pcm.data(),pcm.size(),{0,531,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,64}));
    std::array<std::int16_t,530> prefix{};
    assert(b.render(prefix.data(),prefix.size())==530);
    b.noteOff(); assert(b.transitionStarted());
    std::int16_t out=0; assert(b.next(out));
    assert(out==pcm[530] && b.transitionTruncated() && !b.active());
    assert(b.transitionFramesRendered()==1 && !b.transitionActive());
    b.reset();
    // The current index is outside the blend, but interpolation already touches it.
    assert(b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease,Q/2,{true,32},{true,16}));
    std::array<std::int16_t,997> half_prefix{};
    assert(b.render(half_prefix.data(),half_prefix.size())==997);
    b.noteOff(); assert(b.transitionStarted());
}
void badInputsAndReset() {
    auto pcm=signal(600); Bridge b;
    for(unsigned n:{0u,1u,15u,65u,65535u})
        assert(!b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,static_cast<std::uint16_t>(n)}));
    assert(!b.start(nullptr,600,{0,600,19,531},CursorLoopMode::UntilRelease));
    assert(b.start(pcm.data(),pcm.size(),{510,600,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,32})==false); // invalid region
    assert(b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,32}));
    const auto phase=b.phaseQ32();
    assert(!b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease));
    assert(!b.setStep(0) && b.phaseQ32()==phase);
    assert(b.render(nullptr,50)==0 && b.phaseQ32()==phase);
    std::array<std::int16_t,510> tmp{}; assert(b.render(tmp.data(),0)==0);
    assert(b.render(tmp.data(),tmp.size())==510); b.noteOff(); assert(b.transitionStarted());
    b.reset(); assert(!b.active() && !b.transitionStarted() && !b.transitionTruncated());
    assert(!b.setStep(Q)); b.noteOff();
}
void extremeStepAndBounds() {
    auto pcm=signal(600); Bridge b;
    assert(b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,32}));
    std::array<std::int16_t,510> tmp{}; assert(b.render(tmp.data(),tmp.size())==510);
    b.noteOff(); assert(b.transitionStarted());
    assert(b.setStep(std::numeric_limits<std::uint64_t>::max()));
    std::int16_t out=0; assert(b.next(out));
    assert(out==pcm[510] && !b.active() && b.transitionTruncated());
    // Constant extremes need no bridge and cannot clip or overflow.
    for(auto value:{-32768,32767}) {
        std::fill(pcm.begin(),pcm.end(),static_cast<std::int16_t>(value)); b.reset();
        assert(b.start(pcm.data(),pcm.size(),{0,600,19,531},CursorLoopMode::UntilRelease,Q,{true,32},{true,64}));
        assert(b.render(tmp.data(),tmp.size())==510); b.noteOff();
        assert(!b.transitionStarted()); assert(b.next(out)); assert(out==value);
    }
}
int main() {
    unchanged(); oracleMatrix(); blockSplits(); endAndFraction(); badInputsAndReset(); extremeStepAndBounds();
    std::cout << "Bounded release: 6 suites PASS; default remains disabled\n";
}
