// Compiles against frozen 7a019caf and new macro=0/1 with no installed hook.
#include "gm/ZoneResolver.h"
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
using namespace gm_synth;
void require(bool b){if(!b)std::abort();}
int main(){
    sample::PublishedSampleDirectory directory;sample::ResidentRecord r{};
    std::array<std::int16_t,640> pcm{};
    for(unsigned i=0;i<pcm.size();++i)pcm[i]=static_cast<int>((i*997+143)%50001)-25000;
    r.pcm=pcm.data();r.bank_generation=r.sample_generation=1;r.descriptor.id=0;
    r.descriptor.frames=pcm.size();r.descriptor.pcm_bytes=pcm.size()*2;
    r.descriptor.sample_rate=44100;r.descriptor.loop_start=23;r.descriptor.loop_end=595;r.descriptor.flags=1;
    require(directory.publish(0,&r));
    gm::PlaybackZone z;z.sample_id=0;z.loop_mode=voice::LoopMode::Continuous;z.envelope.release_frames=137;
    gm::PlaybackProgramSnapshot p;p.channel=0;p.token=1;p.bank_generation=1;p.zones=&z;p.zone_count=1;
    gm::PlaybackProgramSnapshotExchange exchange;require(exchange.setInitialActive(&p));
    gm::RealtimeProgramEventApplier events(exchange);voice::VoicePool pool(directory);
    std::uint64_t hash=UINT64_C(14695981039346656037);unsigned frames=0;
    for(unsigned i=0;i<1024;++i){
        realtime::RealtimeEvent e;e.channel=0;e.timestamp_frame=i;e.type=realtime::EventType::NoteOn;e.data1=60;e.data2=100;
        bool apply=false;
        if(i==0||i==61){apply=true;e.data1=i?64:60;}
        if(i==97||i==177){apply=true;e.type=realtime::EventType::ControlChange;e.data1=64;e.data2=i==97?127:0;}
        if(i==127){apply=true;e.type=realtime::EventType::NoteOff;e.data1=60;}
        if(i==211){apply=true;e.type=realtime::EventType::ControlChange;e.data1=7;e.data2=70;}
        if(i==251){apply=true;e.type=realtime::EventType::PitchBend;e.value=10001;}
        if(i==311){apply=true;e.type=realtime::EventType::NoteOff;e.data1=64;}
        if(apply)require(events.apply(e,pool));
        std::array<std::int32_t,2> out{};pool.renderAdd(out.data(),1);
        for(auto v:out){hash^=static_cast<std::uint32_t>(v);hash*=UINT64_C(1099511628211);}
        hash^=pool.activeVoiceCount();hash*=UINT64_C(1099511628211);++frames;
    }
    pool.allSoundOff();require(directory.stats().live_voice_refs==0);
    std::printf("APP_NO_PLAN frames=%u hash=%" PRIu64 " starts=%" PRIu64 "\n",frames,hash,events.stats().voices_started);
}
