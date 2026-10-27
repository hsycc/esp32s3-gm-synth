#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gm_synth::app {

// Control task only. Work units count bounded loop iterations, NOT microseconds
// or CPU cycles. A caller may yield/check its deadline at checkpoints; false
// cancels the entire proposal. No partial candidate is ever published.
enum class LoopAnalysisStatus : std::uint8_t { Complete, InvalidInput, Budget, Cancelled, Numeric };
struct LoopAnalysisBudget {
    using Checkpoint = bool (*)(void*);
    std::uint32_t remaining = 50000, used = 0, since_checkpoint = 0;
    std::uint32_t pcm_reads = 0, checkpoints = 0;
    Checkpoint checkpoint = nullptr;
    void* opaque = nullptr;
    LoopAnalysisStatus failure = LoopAnalysisStatus::Complete;
    bool take(std::uint32_t units) noexcept {
        if (failure != LoopAnalysisStatus::Complete) return false;
        if (units > remaining) { failure = LoopAnalysisStatus::Budget; return false; }
        remaining -= units; used += units; since_checkpoint += units;
        // All callers charge at most 193 units at a time. At most 4288 charged
        // iterations separate callback invocations, independent of loop length.
        if (since_checkpoint >= 4096) {
            since_checkpoint = 0; ++checkpoints;
            if (checkpoint && !checkpoint(opaque)) {
                failure = LoopAnalysisStatus::Cancelled; return false;
            }
        }
        return true;
    }
};

enum LoopRejection : std::uint32_t {
    SeamFloor = 1u << 0, AbsoluteBenefit = 1u << 1, RelativeBenefit = 1u << 2,
    PeriodLimit = 1u << 3, SpectrumUndefined = 1u << 4, SpectrumHigh = 1u << 5,
    LagUndefined = 1u << 6, LagShift = 1u << 7, ChangedPeak = 1u << 8,
    NeighborhoodRms = 1u << 9, FadeTooLong = 1u << 10
};
struct LoopCandidateMetrics {
    std::uint16_t fade = 0;
    std::uint32_t seam = 0, near_peak = 0, changed_peak = 0, unchanged_peak = 0;
    std::uint32_t changed_edges = 0, unchanged_edges = 0, rejected = 0;
    double near_rms = 0, spectral_distance = 0;
    int lag_shift = 0;
    bool spectral_defined = false, lag_defined = false;
};
struct LoopAnalysisResult {
    std::array<LoopCandidateMetrics, 4> candidates{};
    std::uint16_t selected_fade = 0, seam_best_fade = 0;
    bool complete = false, runtime_approved = false;
};

// Bounded port of prepared-loop-review-v3-source-matched-peak. The seven frozen
// thresholds are NOT tunables here. Exact integer seam/peak arithmetic, local
// double-precision DFT and lag diagnostics; not pitch/perceptual guarantees.
// Scratch is caller-owned (~4 KiB), no allocation, whole-loop copy or retained
// PCM pointer. One serialized caller per instance, including across checkpoints.
class BoundedLoopAnalyzer {
public:
    static constexpr std::uint32_t kMaxWorkPerAnalysis = 50000;
    static constexpr const char* kPolicy = "prepared-loop-review-v3-source-matched-peak";

    LoopAnalysisStatus analyze(const std::int16_t* pcm, std::uint32_t n,
                               LoopAnalysisBudget& budget, LoopAnalysisResult& out) noexcept {
        out = {}; // fail closed, including reuse of a previously successful result
        if (!pcm || n < 4) return LoopAnalysisStatus::InvalidInput;
        LoopAnalysisResult result;
        constexpr unsigned fades[] = {0,16,32,64};
        for (unsigned index = 0; index < 4; ++index) {
            auto& c = result.candidates[index]; c.fade = fades[index];
            if (c.fade >= n/2) { c.rejected = FadeTooLong; continue; }
            if (!neighborhood(pcm,n,c,budget) ||
                (c.fade && !spectral(pcm,n,c,budget)))
                return budget.failure == LoopAnalysisStatus::Complete ?
                    LoopAnalysisStatus::Numeric : budget.failure;
        }
        const auto& direct = result.candidates[0];
        unsigned best = 0, selected = 0;
        for (unsigned i = 1; i < 4; ++i) {
            auto& c = result.candidates[i];
            if (c.rejected == FadeTooLong) continue;
            const auto delta = static_cast<int>(direct.seam) - static_cast<int>(c.seam);
            if (direct.seam < 2500) c.rejected |= SeamFloor;
            if (delta < 1800) c.rejected |= AbsoluteBenefit;
            if (static_cast<double>(delta)/std::max<std::uint32_t>(1u,direct.seam) < 0.65)
                c.rejected |= RelativeBenefit;
            if (static_cast<double>(c.fade)/n >= 0.01) c.rejected |= PeriodLimit;
            if (!c.spectral_defined) c.rejected |= SpectrumUndefined;
            else if (c.spectral_distance >= 0.3) c.rejected |= SpectrumHigh;
            if (!c.lag_defined) c.rejected |= LagUndefined;
            else if (c.lag_shift) c.rejected |= LagShift;
            if (c.changed_peak > direct.near_peak) c.rejected |= ChangedPeak;
            if (c.near_rms > direct.near_rms*1.05) c.rejected |= NeighborhoodRms;
            constexpr auto benefit_mask = SeamFloor | AbsoluteBenefit | RelativeBenefit;
            if (!(c.rejected & benefit_mask) && (!best || less(c,result.candidates[best]))) best=i;
            if (!c.rejected && (!selected || less(c,result.candidates[selected]))) selected=i;
        }
        result.seam_best_fade = result.candidates[best].fade;
        result.selected_fade = result.candidates[selected].fade;
        result.complete = true; out = result;
        return LoopAnalysisStatus::Complete;
    }
private:
    struct Point { std::int32_t value; std::uint32_t source; bool mixed; };
    static bool less(const LoopCandidateMetrics& a,const LoopCandidateMetrics& b) noexcept {
        return a.seam < b.seam || (a.seam == b.seam && a.fade < b.fade);
    }
    static Point point(const std::int16_t* pcm,std::uint32_t n,unsigned fade,
                       std::uint64_t position,LoopAnalysisBudget& work) noexcept {
        const auto i = static_cast<std::uint32_t>(position<n ? position :
            fade+(position-n)%(n-fade));
        ++work.pcm_reads;
        if (fade && i >= n-fade) {
            const auto j=i-(n-fade); ++work.pcm_reads;
            const auto value=static_cast<std::int32_t>(pcm[i])*static_cast<int>(fade-j)+
                             static_cast<std::int32_t>(pcm[j])*static_cast<int>(j+1);
            // Denominator is 17/33/65, so an integer numerator never has a
            // half-way tie. This is exactly Python round for these candidates.
            const auto magnitude=(std::abs(value)+static_cast<int>(fade/2))/static_cast<int>(fade+1);
            return {value<0 ? -magnitude : magnitude,i,true};
        }
        return {pcm[i],i,false};
    }
    static bool neighborhood(const std::int16_t* pcm,std::uint32_t n,
                             LoopCandidateMetrics& c,LoopAnalysisBudget& w) noexcept {
        const unsigned radius=std::min<std::uint32_t>(96u,(n-1)/2), edges=radius*2+1;
        if (!w.take(edges)) return false;
        std::uint64_t squares=0;
        auto a=point(pcm,n,c.fade,std::uint64_t{n}-radius-1,w);
        for (std::uint64_t i=std::uint64_t{n}-radius;i<=std::uint64_t{n}+radius;++i) {
            const auto b=point(pcm,n,c.fade,i,w);
            const auto delta=static_cast<unsigned>(std::abs(b.value-a.value));
            squares+=std::uint64_t{delta}*delta; c.near_peak=std::max<std::uint32_t>(c.near_peak,delta);
            if (i==n) c.seam=delta;
            if (!a.mixed && !b.mixed && b.source==std::uint64_t{a.source}+1) {
                ++c.unchanged_edges; c.unchanged_peak=std::max<std::uint32_t>(c.unchanged_peak,delta);
            } else { ++c.changed_edges; c.changed_peak=std::max<std::uint32_t>(c.changed_peak,delta); }
            a=b;
        }
        c.near_rms=std::sqrt(static_cast<double>(squares)/edges);
        return std::isfinite(c.near_rms);
    }
    bool spectrum(const std::array<double,128>& values,unsigned size,
                  std::array<double,64>& out,LoopAnalysisBudget& w) noexcept {
        if (!w.take(size)) return false;
        double mean=0; for (unsigned i=0;i<size;++i) mean+=values[i]; mean/=size;
        constexpr double pi=3.141592653589793238462643383279502884;
        if (!w.take(size)) return false;
        for (unsigned i=0;i<size;++i)
            window_[i]=(values[i]-mean)*(0.5-0.5*std::cos(2*pi*i/(size-1)));
        for (unsigned k=1;k<=size/2;++k) {
            if (!w.take(size)) return false;
            double real=0,imag=0;
            for (unsigned i=0;i<size;++i) {
                const double angle=2*pi*k*i/size;
                real+=window_[i]*std::cos(angle); imag+=window_[i]*std::sin(angle);
            }
            out[k-1]=std::hypot(real,imag);
        }
        return true;
    }
    bool lag(const std::array<double,128>& values,unsigned size,int& out,
             LoopAnalysisBudget& w) noexcept {
        if (!w.take(size)) return false;
        double mean=0; for (unsigned i=0;i<size;++i) mean+=values[i]; mean/=size;
        double best=-2; out=0;
        for (unsigned lag=2;lag<=std::min(size/2,48u);++lag) {
            if (!w.take(size-lag)) return false;
            double a=0,b=0,ab=0;
            for (unsigned i=0;i<size-lag;++i) {
                const double lhs=values[i]-mean,rhs=values[i+lag]-mean;
                a+=lhs*lhs; b+=rhs*rhs; ab+=lhs*rhs;
            }
            const double denominator=std::sqrt(a*b);
            if (denominator>1e-10) {
                const double correlation=ab/denominator;
                // Python max((correlation, lag)) prefers the larger lag on ties.
                if (correlation>=best) { best=correlation; out=static_cast<int>(lag); }
            }
        }
        return true;
    }
    bool spectral(const std::int16_t* pcm,std::uint32_t n,LoopCandidateMetrics& c,
                  LoopAnalysisBudget& w) noexcept {
        const unsigned size=std::min<std::uint32_t>(128u,std::min<std::uint32_t>(n,std::max(32u,unsigned(c.fade)*2)));
        if (!w.take(size)) return false;
        for (unsigned i=0;i<size;++i) {
            original_[i]=pcm[n-size+i]; ++w.pcm_reads;
            modified_[i]=point(pcm,n,c.fade,std::uint64_t{n}-size+i,w).value;
        }
        if (!spectrum(original_,size,sa_,w) || !spectrum(modified_,size,sb_,w)) return false;
        if (!w.take(size/2)) return false;
        double na=0,nb=0;
        for (unsigned i=0;i<size/2;++i) { na+=sa_[i]*sa_[i]; nb+=sb_[i]*sb_[i]; }
        na=std::sqrt(na); nb=std::sqrt(nb);
        if (!std::isfinite(na) || !std::isfinite(nb)) return false;
        if (na<1e-7 || nb<1e-7) return true; // frozen insufficient-signal behavior
        if (!w.take(size/2)) return false;
        double distance=0;
        for (unsigned i=0;i<size/2;++i) { const auto d=sa_[i]/na-sb_[i]/nb; distance+=d*d; }
        distance=std::sqrt(distance);
        if (!std::isfinite(distance)) return false;
        // Frozen Host stores distance rounded to 5 decimal places BEFORE the
        // policy gate. Implement nearest-even rather than C++ round-away.
        const double scaled=distance*100000, lower=std::floor(scaled), fraction=scaled-lower;
        c.spectral_distance=(lower+((fraction>0.5 || (fraction==0.5 &&
            static_cast<unsigned>(lower)%2)) ? 1 : 0))/100000;
        c.spectral_defined=true;
        int a=0,b=0;
        if (!lag(original_,size,a,w) || !lag(modified_,size,b,w)) return false;
        c.lag_defined=a!=0 && b!=0; c.lag_shift=b-a;
        return true;
    }
    std::array<double,128> original_{}, modified_{}, window_{};
    std::array<double,64> sa_{}, sb_{};
};
} // namespace gm_synth::app
