#include "voice/experimental/BoundedLoopReleaseCursor.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gm_synth::voice::experimental;
std::uint64_t number(const char* arg) {
    const std::string text(arg);
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("expected unsigned decimal");
    return std::stoull(text);
}
int main(int argc,char** argv) {
    try {
        if(argc!=10) throw std::invalid_argument("pcm loop_start loop_end fade step off release block mode");
        const auto ls=number(argv[2]),le=number(argv[3]),fade=number(argv[4]);
        const auto step=number(argv[5]),off=number(argv[6]),release=number(argv[7]);
        const auto block=number(argv[8]),mode=number(argv[9]);
        if(ls>1000000 || le>1000000 || fade>64 || step==0 || off>4000000 ||
           release>64 || block<1 || block>257 || (mode!=1 && mode!=3))
            throw std::invalid_argument("probe argument outside bounds");
        std::ifstream f(argv[1],std::ios::binary|std::ios::ate);
        const auto size=f.tellg();
        if(size<8 || size>2000000 || size%2) throw std::invalid_argument("invalid PCM bytes");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        f.seekg(0); f.read(reinterpret_cast<char*>(bytes.data()),size);
        if(!f) throw std::runtime_error("truncated PCM");
        std::vector<std::int16_t> pcm(bytes.size()/2);
        for(std::size_t i=0;i<pcm.size();++i) {
            const int x=bytes[2*i]+256*bytes[2*i+1];
            pcm[i]=static_cast<std::int16_t>(x>=32768 ? x-65536 : x);
        }
        BoundedLoopReleaseCursor cursor;
        if(!cursor.start(pcm.data(),static_cast<std::uint32_t>(pcm.size()),
            {0,static_cast<std::uint32_t>(pcm.size()),static_cast<std::uint32_t>(ls),static_cast<std::uint32_t>(le)},
            mode==1?CursorLoopMode::Continuous:CursorLoopMode::UntilRelease,step,
            {fade!=0,static_cast<std::uint16_t>(fade)},
            {release!=0,static_cast<std::uint16_t>(release)})) throw std::invalid_argument("cursor rejected setup");
        const auto begin=off>4?off-4:0;
        const auto end=off+129;
        std::array<std::int16_t,257> buffer{};
        std::uint64_t t=0,emitted=0;
        while(t<end && cursor.active()) {
            if(t==off) cursor.noteOff();
            auto count=std::min(block,end-t);
            if(t<off) count=std::min(count,off-t);
            const auto got=cursor.render(buffer.data(),static_cast<std::size_t>(count));
            for(std::size_t i=0;i<got;++i) if(t+i>=begin) {
                const auto x=static_cast<std::uint16_t>(buffer[i]);
                std::cout.put(static_cast<char>(x&255)); std::cout.put(static_cast<char>(x>>8)); ++emitted;
            }
            t+=got;
        }
        if(!std::cout) throw std::runtime_error("PCM output failed");
        std::cerr << "{\"started\":" << (cursor.transitionStarted()?"true":"false")
            << ",\"truncated\":" << (cursor.transitionTruncated()?"true":"false")
            << ",\"tail_limited\":" << (cursor.transitionTailLimited()?"true":"false")
            << ",\"planned_frames\":" << cursor.transitionPlannedFrames()
            << ",\"bridge_frames\":" << cursor.transitionFramesRendered()
            << ",\"phase_q32\":" << cursor.phaseQ32()
            << ",\"active\":" << (cursor.active()?"true":"false")
            << ",\"emitted\":" << emitted << "}\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
