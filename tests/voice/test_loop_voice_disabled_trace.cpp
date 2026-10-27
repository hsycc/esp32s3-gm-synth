// Also compiles against the frozen baseline: no experimental API references.
#include "voice/VoiceRenderer.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
namespace v=gm_synth::voice;namespace s=gm_synth::sample;
std::uint64_t hash=1469598103934665603ull;
void add(std::uint64_t n){for(unsigned b=0;b<8;++b){hash^=(n>>(b*8))&255;hash*=1099511628211ull;}}
int main(){
    unsigned cases=0;
    for(auto mode:{v::LoopMode::None,v::LoopMode::Continuous,v::LoopMode::UntilRelease})
    for(auto step:{v::kPhaseOneQ32/2,v::kPhaseOneQ32,5*v::kPhaseOneQ32/4,31*v::kPhaseOneQ32+7})
    for(unsigned profile:{0u,1u,2u})for(bool fast:{false,true}){
        s::PublishedSampleDirectory directory;s::ResidentRecord record{};std::array<std::int16_t,603> pcm{};
        for(unsigned i=0;i<pcm.size();++i)pcm[i]=static_cast<int>((i*997+143)%50001)-25000;
        record.pcm=pcm.data();record.bank_generation=record.sample_generation=1;
        record.descriptor.id=17;record.descriptor.frames=pcm.size();record.descriptor.pcm_bytes=pcm.size()*2;
        record.descriptor.loop_start=23;record.descriptor.loop_end=523;record.descriptor.sample_rate=44100;
        assert(directory.publish(17,&record));v::VoiceStartParams p;
        p.sample_id=17;p.loop_mode=mode;p.sample_region={true,11,573,23,523};p.phase_increment_q32=step;
        p.envelope={3,17,7,31,25000,187};p.gain_left_q15=27000;p.gain_right_q15=15000;
        if(profile)p.lowpass={true,1<<22,1<<23,0,0};
        if(profile==2){p.vibrato_lfo={true,11,19000000,350};p.mod_lfo.enabled=true;
            p.mod_lfo.phase_step_q32=3000000;p.mod_lfo.pitch_depth_cents=-100;
            p.mod_envelope.enabled=true;p.mod_envelope.envelope={0,29,11,91,15000,137};p.mod_envelope.pitch_depth_cents=200;
        }
        v::Voice voice;assert(voice.start(directory,p));
        for(unsigned i=0;i<1700;++i){
            if(i==113){v::VoiceRuntimeControlUpdate u;u.phase_increment_q32=step+7;u.gain_left_q15=12000;
                u.gain_right_q15=19000;u.vibrato_pitch_depth_cents=400;u.chorus_send_units=500;u.reverb_send_units=750;
                assert(voice.applyRuntimeControlUpdate(u)==voice.active());}
            if(i==1001)voice.noteOff();
            std::array<std::int32_t,2> out{5,11};auto r=fast?voice.renderAddRealtime(out.data(),1):voice.renderAdd(out.data(),1);
            add(out[0]);add(out[1]);add(r.frames_rendered);add(r.finished);add(voice.phaseQ32());
            add(voice.envelopeLevelQ15());add(voice.modEnvelopeLevelQ15());add(voice.currentPhaseIncrementQ32());
        }
        voice.reset();assert(directory.stats().live_voice_refs==0);++cases;
    }
    std::cout<<"disabled-trace cases="<<cases<<" hash="<<hash<<"\n";
}
