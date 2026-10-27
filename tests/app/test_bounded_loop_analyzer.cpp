#include "app/BoundedLoopAnalyzer.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace gm_synth::app;
static void require(bool b,int line){if(!b){std::fprintf(stderr,"CORE_FAIL %d\n",line);std::abort();}}
#define CHECK(x) require(bool(x),__LINE__)
int main(){
    BoundedLoopAnalyzer analyzer;
    std::array<std::int16_t,8192> pcm{};
    for(unsigned i=0;i<pcm.size();++i)pcm[i]=static_cast<int>((i*997+143)%50001)-25000;
    LoopAnalysisBudget full;LoopAnalysisResult reference;
    CHECK(analyzer.analyze(pcm.data(),4096,full,reference)==LoopAnalysisStatus::Complete);
    CHECK(full.used<=BoundedLoopAnalyzer::kMaxWorkPerAnalysis);
    for(unsigned cap : {0u,1u,192u,1000u,full.used-1}) {
        LoopAnalysisBudget w;w.remaining=cap;auto out=reference;out.selected_fade=64;
        CHECK(analyzer.analyze(pcm.data(),4096,w,out)==LoopAnalysisStatus::Budget);
        CHECK(!out.complete && out.selected_fade==0 && !out.runtime_approved && w.used<=cap);
    }
    LoopAnalysisBudget exact;exact.remaining=full.used;LoopAnalysisResult out;
    CHECK(analyzer.analyze(pcm.data(),4096,exact,out)==LoopAnalysisStatus::Complete && exact.remaining==0);
    CHECK(out.selected_fade==reference.selected_fade);
    LoopAnalysisBudget cancelled;cancelled.checkpoint=[](void*){return false;};out.selected_fade=64;
    CHECK(analyzer.analyze(pcm.data(),4096,cancelled,out)==LoopAnalysisStatus::Cancelled);
    CHECK(!out.complete && out.selected_fade==0 && cancelled.checkpoints==1);
    LoopAnalysisBudget invalid;
    CHECK(analyzer.analyze(nullptr,4096,invalid,out)==LoopAnalysisStatus::InvalidInput && invalid.used==0);
    CHECK(analyzer.analyze(pcm.data(),3,invalid,out)==LoopAnalysisStatus::InvalidInput);
    // Every short-loop boundary; ASan protects the actual exactly sized view.
    for(unsigned n=4;n<=140;++n) {
        LoopAnalysisBudget w;
        std::vector<std::int16_t> small(pcm.begin(),pcm.begin()+n);
        CHECK(analyzer.analyze(small.data(),n,w,out)==LoopAnalysisStatus::Complete);
        CHECK(out.complete && w.used<=50000 && w.pcm_reads<=2000);
        for(auto c:out.candidates)CHECK((c.rejected==FadeTooLong)==(c.fade>=n/2));
    }
    // Identical endpoint windows but completely different interior and length:
    // same work/read counts and metrics. Period rejection may legitimately vary.
    std::copy_n(pcm.data()+4096-192,192,pcm.data()+8192-192);
    LoopAnalysisBudget long_work;LoopAnalysisResult longer;
    CHECK(analyzer.analyze(pcm.data(),8192,long_work,longer)==LoopAnalysisStatus::Complete);
    CHECK(full.used==long_work.used && full.pcm_reads==long_work.pcm_reads);
    for(unsigned i=0;i<4;++i) {
        const auto& a=reference.candidates[i];const auto& b=longer.candidates[i];
        CHECK(a.seam==b.seam && a.near_rms==b.near_rms && a.changed_peak==b.changed_peak);
        CHECK(a.spectral_distance==b.spectral_distance && a.lag_shift==b.lag_shift);
    }
    std::puts("ANALYSIS_CORE budget/exact-cap/cancel/reset/bounds/length-independent: PASS");
}
