#include "voice/experimental/LoopCrossfadeCursor.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

using namespace gm_synth::voice::experimental;
using Mode = CursorLoopMode;
constexpr auto Q = LoopCrossfadeCursor::kOne;

std::vector<std::int16_t> source(std::size_t n) {
    std::vector<std::int16_t> x(n);
    std::mt19937 rng(731);
    for (auto& v : x) v = static_cast<std::int16_t>(static_cast<int>(rng() % 65536) - 32768);
    return x;
}

// Offline oracle: materialize the complete virtual tail, then use long-double
// interpolation; it intentionally does not call the cursor's blend/phase math.
std::vector<std::int16_t> oracle(const std::vector<std::int16_t>& raw, CursorRegion r,
                                Mode mode, unsigned fade, std::uint64_t step,
                                std::size_t total, std::size_t off) {
    auto edited = raw;
    if (fade) for (unsigned k = 0; k < fade; ++k) {
        const long double v = (static_cast<long double>(raw[r.loop_end-fade+k]) * (fade-k) +
                               static_cast<long double>(raw[r.loop_start+k]) * (k+1)) / (fade+1);
        edited[r.loop_end-fade+k] = static_cast<std::int16_t>(std::round(v));
    }
    std::vector<std::int16_t> out;
    std::uint64_t phase = std::uint64_t{r.start} << 32;
    for (std::size_t frame = 0; frame < total && phase < (std::uint64_t{r.end} << 32); ++frame) {
        const bool loop = mode == Mode::Continuous || (mode == Mode::UntilRelease && frame < off);
        const auto& data = loop ? edited : raw;
        const auto a = static_cast<std::uint32_t>(phase >> 32);
        auto b = a + 1;
        if (loop && a >= r.loop_start && b >= r.loop_end) b = r.loop_start + fade;
        else if (b >= r.end) b = r.end - 1;
        const long double f = static_cast<std::uint32_t>(phase) / static_cast<long double>(Q);
        out.push_back(static_cast<std::int16_t>(std::floor(data[a] + (data[b]-data[a])*f)));
        phase += step; // Test ranges here cannot overflow; extreme steps have a separate test.
        if (loop) while (phase >= (std::uint64_t{r.loop_end} << 32))
            phase -= std::uint64_t{r.loop_end - r.loop_start - fade} << 32;
    }
    return out;
}

std::vector<std::int16_t> run(const std::vector<std::int16_t>& pcm, CursorRegion r,
                            Mode mode, ExperimentalFade fade, std::uint64_t step,
                            std::size_t total, std::size_t off, std::size_t block) {
    LoopCrossfadeCursor c;
    assert(c.start(pcm.data(), static_cast<std::uint32_t>(pcm.size()), r, mode, step, fade));
    std::vector<std::int16_t> out(total, 12345);
    std::size_t done = 0;
    while (done < total && c.active()) {
        if (done == off) { c.noteOff(); c.noteOff(); }
        auto count = std::min(block, total - done);
        if (done < off) count = std::min(count, off - done);
        const auto n = c.render(out.data()+done, count);
        assert(n != 0);
        done += n;
    }
    if (done < total) assert(out[done] == 12345);
    out.resize(done);
    return out;
}

void defaults() {
    LoopCrossfadeCursor c;
    std::int16_t x = 123;
    assert(!c.active() && !c.next(x) && x == 123);
    auto pcm = source(512);
    CursorRegion r{3, 500, 10, 450};
    const auto a = run(pcm, r, Mode::Continuous, {}, Q, 1400, 9999, 64);
    assert(a == run(pcm, r, Mode::Continuous, {false,64}, Q, 1400, 9999, 17));
    assert(a == oracle(pcm, r, Mode::Continuous, 0, Q, 1400, 9999));
}

void matrix() {
    auto pcm = source(587);
    const CursorRegion r{11, 573, 19, 531};
    const std::array<std::uint64_t, 6> steps{Q/2, Q, Q+Q/4, 3*Q+Q/8, 23*Q, 700*Q+9};
    for (const auto mode : {Mode::None, Mode::Continuous, Mode::UntilRelease})
        for (unsigned f : {0u,16u,32u,64u}) {
            if (mode == Mode::None && f) continue;
            for (auto step : steps) for (std::size_t off : {0u, 99u, 401u, 4096u}) {
                const auto expected = oracle(pcm,r,mode,f,step,2048,off);
                for (std::size_t block : {1u,7u,63u,64u,65u,2048u})
                    assert(run(pcm,r,mode,{f!=0,static_cast<std::uint16_t>(f)},step,2048,off,block)==expected);
            }
        }
}

void release_positions() {
    auto pcm = source(288);
    const CursorRegion r{0,288,16,256};
    // Before blend, at entry, inside, just before wrap, just after wrap, later lap.
    for (std::size_t off : {210u,224u,225u,239u,255u,256u,257u,450u,1000u}) {
        auto a = run(pcm,r,Mode::UntilRelease,{true,32},Q,1600,off,64);
        assert(a == oracle(pcm,r,Mode::UntilRelease,32,Q,1600,off));
        assert(a == run(pcm,r,Mode::UntilRelease,{true,32},Q,1600,off,1));
    }
    // Continuous is unaffected by NoteOff at the transport layer (envelope elsewhere).
    assert(run(pcm,r,Mode::Continuous,{true,32},Q,1600,227,64)==
           run(pcm,r,Mode::Continuous,{true,32},Q,1600,9999,64));
}

void overflow_and_many_wraps() {
    std::array<std::int16_t,200> pcm{};
    for (unsigned fade : {0u,16u,32u,64u}) {
        LoopCrossfadeCursor c;
        const auto step = std::numeric_limits<std::uint64_t>::max();
        assert(c.start(pcm.data(),200,{0,200,10,180},Mode::Continuous,step,
                       {fade!=0,static_cast<std::uint16_t>(fade)}));
        const std::uint64_t resume=std::uint64_t{10+fade}<<32, end=180*Q;
        std::uint64_t phase=0;
        for (int k=0;k<1000;++k) {
            std::int16_t x;
            assert(c.next(x));
            phase=resume+(step-(end-phase))%(end-resume);
            assert(c.phaseQ32()==phase && c.phaseQ32()<end);
        }
    }
    LoopCrossfadeCursor one;
    assert(one.start(pcm.data(),200,{2,199,0,0},Mode::None,std::numeric_limits<std::uint64_t>::max()));
    std::int16_t v;
    assert(one.next(v) && !one.active());
}

void validation_transaction() {
    auto pcm=source(512);
    LoopCrossfadeCursor c;
    const CursorRegion good{0,512,10,500};
    assert(!c.start(nullptr,512,good,Mode::Continuous));
    assert(!c.start(pcm.data(),512,good,Mode::Continuous,0));
    assert(!c.start(pcm.data(),512,{9,9,0,0},Mode::None));
    assert(!c.start(pcm.data(),512,{0,513,0,0},Mode::None));
    assert(!c.start(pcm.data(),512,{10,512,0,500},Mode::Continuous));
    assert(!c.start(pcm.data(),512,good,static_cast<Mode>(2)));
    assert(!c.start(pcm.data(),512,good,Mode::None,Q,{true,16}));
    for (unsigned f : {0u,1u,15u,17u,128u,65535u})
        assert(!c.start(pcm.data(),512,good,Mode::Continuous,Q,{true,static_cast<std::uint16_t>(f)}));
    assert(!c.start(pcm.data(),512,{0,512,10,42},Mode::Continuous,Q,{true,16}));
    assert(c.start(pcm.data(),512,good,Mode::Continuous));
    assert(!c.start(pcm.data(),512,good,Mode::Continuous));
    const auto phase=c.phaseQ32();
    assert(c.render(nullptr,1)==0 && c.phaseQ32()==phase);
    std::int16_t out=111;
    assert(c.render(&out,0)==0 && out==111 && c.phaseQ32()==phase);
    assert(!c.setStep(0));
    c.reset();
    assert(!c.active() && c.phaseQ32()==0 && c.fadeFrames()==0);
    assert(c.start(pcm.data(),512,good,Mode::Continuous,Q,{true,32}));
    c.reset();
    assert(c.start(pcm.data(),512,good,Mode::Continuous));
    assert(c.fadeFrames()==0);
}

void step_change_and_fraction() {
    auto pcm=source(512);
    LoopCrossfadeCursor a,b;
    assert(a.start(pcm.data(),512,{0,512,0,512},Mode::Continuous,Q,{true,32}));
    assert(b.start(pcm.data(),512,{0,512,0,512},Mode::Continuous,Q,{true,32}));
    for (int i=0;i<3000;++i) {
        const auto step = i%3==0 ? Q/2 : 7*Q+Q/8;
        assert(a.setStep(step) && b.setStep(step));
        std::int16_t x,y;
        assert(a.next(x) && b.render(&y,1)==1 && x==y);
        assert(a.phaseQ32()==b.phaseQ32());
    }
}

void pcm_extremes_and_tiny_regions() {
    for (std::int16_t v : {std::int16_t{-32768}, std::int16_t{32767},std::int16_t{-1}}) {
        std::vector<std::int16_t> pcm(512,v);
        for (unsigned f : {16u,32u,64u}) {
            auto out=run(pcm,{0,512,0,512},Mode::Continuous,{true,static_cast<std::uint16_t>(f)},Q/2,4000,9999,64);
            assert(std::all_of(out.begin(),out.end(),[v](auto x){return x==v;}));
        }
    }
    const std::vector<std::int16_t> one{-32768};
    assert(run(one,{0,1,0,1},Mode::Continuous,{},Q/2,33,9999,64)==std::vector<std::int16_t>(33,-32768));
    assert(run(one,{0,1,0,0},Mode::None,{},Q/2,33,9999,64)==std::vector<std::int16_t>(2,-32768));
}

int main() {
    defaults(); matrix(); release_positions(); overflow_and_many_wraps();
    validation_transaction(); step_change_and_fraction(); pcm_extremes_and_tiny_regions();
    std::cout << "Loop cursor: 7 suites PASS (1296 oracle/block cases plus boundary tests)\n";
}
