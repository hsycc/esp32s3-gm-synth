// Host transport only: input allocation is NOT part of the embedded analyzer.
#include "app/BoundedLoopAnalyzer.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
int main() {
    gm_synth::app::BoundedLoopAnalyzer analyzer;
    for (;;) {
        unsigned char header[4];
        if (!std::cin.read(reinterpret_cast<char*>(header),4)) return std::cin.eof() && std::cin.gcount()==0 ? 0 : 2;
        const std::uint32_t n=std::uint32_t{header[0]}|(std::uint32_t{header[1]}<<8)|
            (std::uint32_t{header[2]}<<16)|(std::uint32_t{header[3]}<<24);
        if (n<4 || n>1000000) return 2;
        std::vector<std::int16_t> pcm(n);
        for (auto& sample:pcm) {
            unsigned char b[2]; if(!std::cin.read(reinterpret_cast<char*>(b),2)) return 2;
            const auto u=std::uint32_t{b[0]}|(std::uint32_t{b[1]}<<8);
            sample=static_cast<std::int16_t>(u<32768 ? static_cast<int>(u) : static_cast<int>(u)-65536);
        }
        gm_synth::app::LoopAnalysisBudget work;
        gm_synth::app::LoopAnalysisResult r;
        if(analyzer.analyze(pcm.data(),n,work,r)!=gm_synth::app::LoopAnalysisStatus::Complete) return 3;
        std::cout<<std::setprecision(17)<<"{\"selected\":"<<r.selected_fade<<",\"best\":"<<r.seam_best_fade
            <<",\"work\":"<<work.used<<",\"reads\":"<<work.pcm_reads<<",\"runtime_approved\":false,\"candidates\":[";
        bool first=true;
        for (auto c:r.candidates) {
            if (!first) std::cout<<',';
            first=false;
            std::cout<<"{\"fade\":"<<c.fade<<",\"seam\":"<<c.seam<<",\"near_peak\":"<<c.near_peak
                <<",\"changed_peak\":"<<c.changed_peak<<",\"unchanged_peak\":"<<c.unchanged_peak
                <<",\"changed_edges\":"<<c.changed_edges<<",\"unchanged_edges\":"<<c.unchanged_edges
                <<",\"near_rms\":"<<c.near_rms<<",\"spectral_distance\":";
            if(c.spectral_defined) std::cout<<c.spectral_distance;else std::cout<<"null";
            std::cout<<",\"lag_shift\":";if(c.lag_defined)std::cout<<c.lag_shift;else std::cout<<"null";
            std::cout<<",\"rejected\":"<<c.rejected<<'}';
        }
        std::cout<<"]}\n";
    }
}
