#include "voice/VoiceRenderer.h"
#include "voice/GlobalEffectsProcessor.h"
#include "voice/experimental/PreparedLoopHandoff.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#if !GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
#error integration tests require the explicit build opt-in
#endif
namespace v = gm_synth::voice;
namespace s = gm_synth::sample;
namespace x = v::experimental;
constexpr auto Q = v::kPhaseOneQ32;

struct Fixture {
    s::PublishedSampleDirectory directory;
    std::vector<std::int16_t> pcm;
    s::ResidentRecord record{};
    Fixture() : pcm(603) {
        for (std::size_t i=0;i<pcm.size();++i)
            pcm[i]=static_cast<std::int16_t>(static_cast<int>((i*997+143)%50001)-25000);
        init(record,pcm.data(),pcm.size(),1);
        assert(directory.publish(17,&record));
    }
    void init(s::ResidentRecord& r,const std::int16_t* data,std::size_t size,unsigned generation) {
        r.bank_generation=1;r.sample_generation=generation;r.pcm=data;
        r.descriptor.id=17;r.descriptor.frames=size;r.descriptor.pcm_bytes=size*2;
        r.descriptor.sample_rate=44100;r.descriptor.flags=1;
        r.descriptor.loop_start=23;r.descriptor.loop_end=523;
    }
    x::LoopPlanContext context(v::LoopMode mode) const {
        x::LoopPlanContext c;c.asset_sha256.fill(0xa5);c.session_serial=9;
        c.bank_generation=1;c.sample_generation=1;c.sample_id=17;
        c.sample_frames=pcm.size();c.sample_rate=44100;c.noteon_addresses_fixed=true;
        c.region={11,573,23,523};
        c.mode=mode==v::LoopMode::Continuous?x::CursorLoopMode::Continuous:x::CursorLoopMode::UntilRelease;
        return c;
    }
    v::VoiceStartParams params(v::LoopMode mode,std::uint64_t step,unsigned profile) const {
        v::VoiceStartParams p;p.sample_id=17;p.channel=2;p.loop_mode=mode;
        p.sample_region={true,11,573,23,523};p.phase_increment_q32=step;
        p.envelope={3,11,5,23,23000,137};p.gain_left_q15=27000;p.gain_right_q15=17000;
        if (profile) p.lowpass={true,1<<22,1<<23,0,0};
        if (profile==2) {
            p.mod_lfo.enabled=true;p.mod_lfo.delay_frames=7;p.mod_lfo.phase_step_q32=35000000;
            p.mod_lfo.filter_depth_cents=300;p.mod_lfo.gain_table.enabled=true;
            p.mod_envelope.enabled=true;p.mod_envelope.envelope={5,19,7,31,19000,79};
            p.mod_envelope.filter_depth_cents=200;
            p.dynamic_lowpass_table.enabled=true;p.dynamic_lowpass_table.min_offset_cents=-300;
            p.dynamic_lowpass_table.max_offset_cents=500;
            for (std::size_t i=0;i<v::kLfoControlPointCount;++i) {
                p.mod_lfo.gain_table.points[i]={static_cast<int>(18000+1000*i),static_cast<int>(11000+1200*i)};
                p.dynamic_lowpass_table.points[i]={true,static_cast<int>((1<<21)+30000*i),static_cast<int>((1<<22)+60000*i),0,0};
            }
            p.realtime_control.cc21_to_filter_fc_cents=100;
        }
        return p;
    }
    void bind(x::PinnedLoopPlan& plan,x::LoopPlanContext c,unsigned fade,unsigned release) {
        x::LoopPlanRequest r;r.context=c;r.experimental_opt_in=true;
        r.fade={fade!=0,static_cast<std::uint16_t>(fade)};
        r.release={release!=0,static_cast<std::uint16_t>(release)};
        assert(plan.bind(directory,r,c)==x::LoopBindingStatus::Ok);
    }
    void clean() const {
        assert(directory.stats().live_voice_refs==0);
        assert(directory.stats().stale_release_faults==0);
    }
};
v::VoiceStartParams requested(v::VoiceStartParams p,const x::PinnedLoopPlan& plan,const x::LoopPlanContext& c) {
    p.experimental_loop_opt_in=true;p.experimental_loop_plan=&plan;p.experimental_loop_context=&c;return p;
}

// The frozen transport supplies pre-rendered raw PCM. A separate ordinary
// production Voice consumes that finite stream at unity rate with the SAME
// envelope/filter/gain configuration. This checks placement and state ordering
// of the new source hook, not a new perceptual or independently proved kernel.
std::vector<std::int16_t> stream(Fixture& f,x::LoopPlanContext c,unsigned fade,unsigned release,
                               std::uint64_t step,unsigned off,unsigned total=1800) {
    x::BoundedLoopReleaseCursor raw;
    assert(raw.start(f.pcm.data(),f.pcm.size(),c.region,c.mode,step,
                     {fade!=0,static_cast<std::uint16_t>(fade)},
                     {release!=0,static_cast<std::uint16_t>(release)}));
    std::vector<std::int16_t> out;
    for(unsigned i=0;i<total;++i) {
        if(i==off) raw.noteOff();
        std::int16_t sample=0;if(!raw.next(sample))break;out.push_back(sample);
    }
    return out;
}
void downstream_matrix() {
    unsigned cases=0;
    for(auto mode:{v::LoopMode::Continuous,v::LoopMode::UntilRelease})
    for(unsigned fade:{16u,32u,64u}) for(unsigned release:{0u,16u,32u,64u})
    for(auto step:{Q/2,Q,5*Q/4}) for(unsigned profile:{0u,1u,2u}) {
        Fixture f;auto c=f.context(mode);x::PinnedLoopPlan plan;f.bind(plan,c,fade,release);
        const unsigned off=static_cast<unsigned>(((523-fade/2-11)*Q+step-1)/step);
        auto values=stream(f,c,fade,release,step,off);
        s::PublishedSampleDirectory offline;s::ResidentRecord r{};f.init(r,values.data(),values.size(),1);
        r.descriptor.loop_start=r.descriptor.loop_end=0;assert(offline.publish(17,&r));
        auto p=f.params(mode,step,profile);auto refp=p;
        refp.sample_region={};refp.phase_increment_q32=Q;refp.loop_mode=v::LoopMode::None;
        v::Voice generic,realtime,reference;
        assert(generic.start(f.directory,requested(p,plan,c)));
        assert(realtime.start(f.directory,requested(p,plan,c)));
        assert(reference.start(offline,refp));
        assert(f.directory.stats().live_voice_refs==3);
        assert(f.directory.unpublish(17)==&f.record);assert(plan.reset());
        assert(!f.directory.canReclaim(&f.record));
        unsigned done=0;
        for(auto block:{1u,17u,64u,3u,65u,31u,7u,128u,64u,19u,256u,256u,256u,256u}) {
            unsigned left=block;
            while(left && done<1800) {
                if(done==off){generic.noteOff();realtime.noteOff();reference.noteOff();}
                if(done==67) {
                    v::VoiceRuntimeControlUpdate u;u.phase_increment_q32=step;
                    u.gain_left_q15=19000;u.gain_right_q15=23000;u.cc21_filter_offset_cents=90;
                    assert(generic.applyRuntimeControlUpdate(u));assert(realtime.applyRuntimeControlUpdate(u));
                    u.phase_increment_q32=Q;assert(reference.applyRuntimeControlUpdate(u));
                }
                if(done==211) {
                    const bool result=generic.applyCc21FilterOffset(-70);
                    assert(realtime.applyCc21FilterOffset(-70)==result);
                    assert(reference.applyCc21FilterOffset(-70)==result);
                }
                unsigned count=std::min(left,1800-done);
                if(done<off)count=std::min(count,off-done);
                for(unsigned event:{67u,211u})if(done<event)count=std::min(count,event-done);
                std::vector<std::int32_t> a(count*2,7),b(count*2,7),expected(count*2,7);
                const auto ra=generic.renderAdd(a.data(),count),rb=realtime.renderAddRealtime(b.data(),count);
                const auto rr=reference.renderAdd(expected.data(),count);
                assert(a==b && a==expected);assert(ra.frames_rendered==rb.frames_rendered && ra.frames_rendered==rr.frames_rendered);
                assert(ra.finished==rb.finished && ra.finished==rr.finished);
                assert(generic.phaseQ32()==realtime.phaseQ32());
                assert(generic.envelopeLevelQ15()==reference.envelopeLevelQ15());
                assert(generic.modEnvelopeLevelQ15()==reference.modEnvelopeLevelQ15());
                done+=count;left-=count;
            }
        }
        generic.reset();realtime.reset();reference.reset();f.clean();
        assert(offline.stats().live_voice_refs==0 && f.directory.canReclaim(&f.record));++cases;
    }
    assert(cases==216);std::cout<<"Voice source/filter/envelope matrix: "<<cases<<" exact cases PASS\n";
}

void rejection_and_ownership() {
    Fixture f;auto c=f.context(v::LoopMode::Continuous);auto p=f.params(v::LoopMode::Continuous,Q,0);
    x::PinnedLoopPlan plan;f.bind(plan,c,32,32);
    for(unsigned i=0;i<12;++i){
        auto bad=requested(p,plan,c);auto wrong=c;v::Voice target;
        switch(i){
        case 0:bad.experimental_loop_opt_in=false;break;
        case 1:bad.experimental_loop_plan=nullptr;break;
        case 2:bad.experimental_loop_context=nullptr;break;
        case 3:wrong.sample_generation++;break;
        case 4:wrong.asset_sha256[3]^=1;break;
        case 5:bad.sample_region.start_frame++;break;
        case 6:bad.sample_region.loop_start_frame++;break;
        case 7:bad.sample_region.end_frame--;break;
        case 8:bad.loop_mode=v::LoopMode::UntilRelease;break;
        case 9:bad.phase_increment_q32=0;break;
        case 10:bad.sample_id=18;break;
        case 11:wrong.session_serial++;break;
        }
        if(i==3||i==4||i==11)bad.experimental_loop_context=&wrong;
        assert(!target.start(f.directory,bad));assert(!target.active());
        assert(f.directory.stats().live_voice_refs==1);
    }
    v::Voice a;assert(a.start(f.directory,requested(p,plan,c)));assert(a.experimentalLoopEnabled());
    assert(!a.start(f.directory,requested(p,plan,c)));assert(f.directory.stats().live_voice_refs==2);
    assert(f.directory.unpublish(17)==&f.record);
    s::ResidentRecord newer{};f.init(newer,f.pcm.data(),f.pcm.size(),1); // same fields, DIFFERENT pointer
    assert(f.directory.publish(17,&newer));v::Voice stale;
    assert(!stale.start(f.directory,requested(p,plan,c)));assert(newer.voice_refs.load()==0);
    assert(plan.reset());assert(f.directory.stats().live_voice_refs==1);
    std::array<std::int32_t,128> out{};assert(a.renderAddRealtime(out.data(),64).frames_rendered==64);
    a.reset();f.clean();assert(f.directory.canReclaim(&f.record));
    assert(f.directory.unpublish(17)==&newer);
    f.pcm.clear();f.pcm.shrink_to_fit();assert(a.renderAddRealtime(out.data(),64).finished);
    a.noteOff();a.reset();f.clean();
    std::cout<<"Voice invalid/stale/mismatched plan and retired PCM: PASS\n";
}

void dynamic_pitch() {
    unsigned cases=0;
    for(unsigned fade:{16u,32u,64u}) {
        Fixture f;auto c=f.context(v::LoopMode::Continuous);x::PinnedLoopPlan plan;f.bind(plan,c,fade,0);
        auto p=f.params(v::LoopMode::Continuous,Q,0);p.envelope={0,0,0,0,v::kQ15Unity,64};
        p.gain_left_q15=p.gain_right_q15=v::kQ15Unity;
        p.vibrato_lfo={true,7,9000000,1200};
        p.mod_lfo.enabled=true;p.mod_lfo.phase_step_q32=17000000;p.mod_lfo.pitch_depth_cents=-300;
        p.mod_envelope.enabled=true;p.mod_envelope.envelope={0,13,7,37,16000,91};p.mod_envelope.pitch_depth_cents=400;
        v::Voice actual;assert(actual.start(f.directory,requested(p,plan,c)));
        x::BoundedLoopReleaseCursor expected;
        assert(expected.start(f.pcm.data(),f.pcm.size(),c.region,c.mode,Q,{true,static_cast<std::uint16_t>(fade)}));
        for(unsigned i=0;i<2500;++i) {
            if(i==100 || i==311) {
                v::VoiceRuntimeControlUpdate u;u.phase_increment_q32=i==100?Q/3:Q*3;
                u.vibrato_pitch_depth_cents=700;assert(actual.applyRuntimeControlUpdate(u));
            }
            std::array<std::int32_t,2> out{};assert(actual.renderAddRealtime(out.data(),1).frames_rendered==1);
            assert(expected.setStep(actual.currentPhaseIncrementQ32()));std::int16_t sample=0;assert(expected.next(sample));
            assert(out[0]==sample && out[1]==sample && actual.phaseQ32()==expected.phaseQ32());
        }
        actual.reset();plan.reset();f.clean();++cases;
    }
    std::cout<<"Voice modulated pitch source advance: "<<cases<<" x 2500 frames PASS\n";
}

void pool_sustain_effects_and_stops() {
    unsigned cases=0;
    for(auto mode:{v::LoopMode::Continuous,v::LoopMode::UntilRelease})for(unsigned profile:{0u,2u}) {
        Fixture f;auto c=f.context(mode);x::PinnedLoopPlan plan;f.bind(plan,c,32,32);
        constexpr unsigned pedal_up=501;
        auto values=stream(f,c,32,32,Q,pedal_up,4096);
        s::PublishedSampleDirectory offline;s::ResidentRecord record{};f.init(record,values.data(),values.size(),1);
        assert(offline.publish(17,&record));
        auto p=f.params(mode,Q,profile);p.envelope={0,0,0,0,24000,120};p.reverb_send_units=730;p.chorus_send_units=500;
        auto expected=p;expected.sample_region={};expected.loop_mode=v::LoopMode::None;
        v::VoicePool actual_pool(f.directory),expected_pool(offline);
        assert(actual_pool.start(requested(p,plan,c)));assert(expected_pool.start(expected));
        assert(plan.reset());assert(f.directory.unpublish(17)==&f.record);
        v::GlobalEffectsProcessor actual_fx,expected_fx;
        unsigned done=0;bool heard_wet=false;
        while(done<4096) {
            if(done==480){assert(actual_pool.noteOff(2,60,true)==1);assert(expected_pool.noteOff(2,60,true)==1);}
            if(done==pedal_up){assert(actual_pool.releaseSustain(2)==1);assert(expected_pool.releaseSustain(2)==1);}
            if(done==128) {
                v::VoiceRuntimeControlUpdate u;u.gain_left_q15=19000;u.gain_right_q15=23000;
                u.chorus_send_units=230;u.reverb_send_units=850;u.cc21_filter_offset_cents=110;
                const auto update=[](void* p,v::Voice& voice){return voice.applyRuntimeControlUpdate(*static_cast<v::VoiceRuntimeControlUpdate*>(p));};
                assert(actual_pool.updateActiveVoices(2,update,&u)==1);
                assert(expected_pool.updateActiveVoices(2,update,&u)==1);
            }
            unsigned n=std::min(64u,4096-done);
            for(unsigned event:{128u,480u,pedal_up})if(done<event)n=std::min(n,event-done);
            std::array<std::int32_t,128> ad{},ar{},ac{},ed{},er{},ec{};
            actual_pool.renderAddWithEffects(ad.data(),ar.data(),ac.data(),n);
            expected_pool.renderAddWithEffects(ed.data(),er.data(),ec.data(),n);
            assert(ad==ed && ar==er && ac==ec);
            const auto dry=ad;
            actual_fx.processAdd(ac.data(),ar.data(),ad.data(),n);expected_fx.processAdd(ec.data(),er.data(),ed.data(),n);
            assert(ad==ed);heard_wet=heard_wet||(ad!=dry);done+=n;
        }
        assert(heard_wet);assert(actual_pool.activeVoiceCount()==0);assert(expected_pool.activeVoiceCount()==0);
        actual_pool.allSoundOff();expected_pool.allSoundOff();f.clean();assert(offline.stats().live_voice_refs==0);++cases;
    }
    // Fast-release/exclusive/CC120 preserve ownership under crossfade as well.
    for(unsigned kind:{0u,1u,2u,3u}) {
        Fixture f;auto c=f.context(v::LoopMode::UntilRelease);x::PinnedLoopPlan plan;f.bind(plan,c,64,32);
        auto p=f.params(v::LoopMode::UntilRelease,Q,0);p.envelope={0,0,0,0,32767,128};
        v::VoicePool pool(f.directory);assert(pool.start(requested(p,plan,c)));
        if(kind==0){assert(pool.allNotesOff(2,false)==1);}else if(kind==1){assert(pool.allSoundOff(2)==1);}
        else pool.updateActiveVoices(2,[](void* ptr,v::Voice& voice){
            return *static_cast<unsigned*>(ptr)==2?voice.beginFastRelease(17):voice.beginExclusiveRelease(19);
        },&kind);
        std::array<std::int32_t,1024> out{};pool.renderAdd(out.data(),512);pool.allSoundOff();
        plan.reset();f.clean();
    }
    std::cout<<"VoicePool sustain/dry/sends/global FX: "<<cases<<" exact cases; forced stop cases PASS\n";
}

int main(){
    downstream_matrix();rejection_and_ownership();dynamic_pitch();pool_sustain_effects_and_stops();
    std::cout<<"Production Voice opt-in integration: 4 suites PASS; no app enablement\n";
}
