// Test-only binary PCM16-LE adapter. Heap/I/O are confined to this Host harness.
#include "voice/experimental/LoopCrossfadeCursor.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gm_synth::voice::experimental;

std::uint64_t number(const char* text) {
    const std::string s(text);
    std::uint64_t n=0;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),n);
    if (parsed.ec!=std::errc{} || parsed.ptr!=s.data()+s.size()) throw std::runtime_error("invalid integer");
    return n;
}
int main(int argc,char** argv) {
    try {
        if (argc!=8) throw std::runtime_error("usage: probe <pcm16le> <fade> <step_q32> <output_frames> <block> <off_frame> <mode:1|3>");
        const auto fade=number(argv[2]), step=number(argv[3]), total=number(argv[4]);
        const auto block=number(argv[5]), off=number(argv[6]), mode=number(argv[7]);
        if ((fade!=0 && fade!=16 && fade!=32 && fade!=64) || !step || !total || total>4000000 ||
            !block || block>2048 || (mode!=1 && mode!=3)) throw std::runtime_error("argument outside bounds");
        std::ifstream f(argv[1],std::ios::binary|std::ios::ate);
        if (!f) throw std::runtime_error("cannot read PCM");
        const auto bytes=f.tellg();
        if (bytes<8 || bytes>2000000 || bytes%2) throw std::runtime_error("PCM size outside bounds");
        std::vector<char> raw(static_cast<std::size_t>(bytes));
        f.seekg(0); f.read(raw.data(),bytes);
        if (!f) throw std::runtime_error("truncated PCM");
        std::vector<std::int16_t> pcm(raw.size()/2);
        for (std::size_t i=0;i<pcm.size();++i) {
            const auto u=static_cast<unsigned char>(raw[2*i]) | (static_cast<unsigned>(static_cast<unsigned char>(raw[2*i+1]))<<8);
            pcm[i]=static_cast<std::int16_t>(u<32768 ? static_cast<int>(u) : static_cast<int>(u)-65536);
        }
        std::vector<std::int16_t> buffer(block);
        std::vector<char> encoded(block*2);
        LoopCrossfadeCursor cursor;
        const auto n=static_cast<std::uint32_t>(pcm.size());
        if (!cursor.start(pcm.data(),n,{0,n,0,n},mode==1?CursorLoopMode::Continuous:CursorLoopMode::UntilRelease,
                          step,{fade!=0,static_cast<std::uint16_t>(fade)})) throw std::runtime_error("cursor rejected geometry");
        std::uint64_t done=0;
        while (done<total && cursor.active()) {
            if (done==off) cursor.noteOff();
            auto count=std::min(block,total-done);
            if (done<off) count=std::min(count,off-done);
            const auto got=cursor.render(buffer.data(),count);
            if (!got) throw std::runtime_error("cursor made no progress");
            for (std::size_t i=0;i<got;++i) {
                const auto u=static_cast<std::uint16_t>(buffer[i]);
                encoded[2*i]=static_cast<char>(u&255); encoded[2*i+1]=static_cast<char>(u>>8);
            }
            std::cout.write(encoded.data(),static_cast<std::streamsize>(got*2));
            done+=got;
        }
        if (!std::cout) throw std::runtime_error("output failed");
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
