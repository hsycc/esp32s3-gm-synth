// A deterministic trace compiled against pre-output main AND the current
// default / optional ABI. No output hook is installed. This compares whole PCM
// and historical counters, including clipping, FX, scheduling and master gain.
#include "gm/ZoneResolver.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
using namespace gm_synth;
void require(bool ok) { if (!ok) std::abort(); }
int main() {
    sample::PublishedSampleDirectory directory;
    sample::ResidentRecord record{};
    std::array<std::int16_t,640> pcm{};
    for (unsigned i=0;i<pcm.size();++i) pcm[i]=static_cast<int>((i*997+143)%50001)-25000;
    record.pcm=pcm.data(); record.bank_generation=record.sample_generation=1;
    record.descriptor.id=0; record.descriptor.frames=pcm.size(); record.descriptor.pcm_bytes=pcm.size()*2;
    record.descriptor.sample_rate=44100; record.descriptor.loop_start=23; record.descriptor.loop_end=595;
    record.descriptor.flags=1;
    require(directory.publish(0,&record));
    gm::PlaybackZone zone; zone.sample_id=0; zone.loop_mode=voice::LoopMode::Continuous;
    zone.envelope.release_frames=137;
    gm::PlaybackProgramSnapshot program;
    program.channel=0; program.token=1; program.bank_generation=1; program.zones=&zone; program.zone_count=1;
    gm::PlaybackProgramSnapshotExchange exchange; require(exchange.setInitialActive(&program));
    gm::RealtimeProgramEventApplier events(exchange); voice::VoicePool pool(directory);
    realtime::RealtimeEventQueue queue; realtime::BlockEventScheduler scheduler(queue);
    realtime::ScheduledAudioBlockRenderer renderer(pool,scheduler,&gm::RealtimeProgramEventApplier::callback,&events);
    auto push=[&](unsigned frame,realtime::EventType type,unsigned a,unsigned b=0) {
        realtime::RealtimeEvent event{}; event.timestamp_frame=frame; event.type=type;
        event.data1=a; event.data2=b; event.value=10001;
        require(queue.tryPush(event));
    };
    push(0,realtime::EventType::ControlChange,91,40);
    push(0,realtime::EventType::ControlChange,93,20);
    for(unsigned key=48;key<72;++key) push(7,realtime::EventType::NoteOn,key,127);
    push(12345,realtime::EventType::PitchBend,0);
    push(30000,realtime::EventType::ControlChange,64,127);
    for(unsigned key=48;key<72;++key) push(31111,realtime::EventType::NoteOff,key);
    push(40001,realtime::EventType::ControlChange,64,0);
    std::uint64_t hash=UINT64_C(14695981039346656037);
    const std::array<std::int32_t,5> masters{{32767,16383,0,32766,8192}};
    for(unsigned block=0;block<1024;++block) {
        require(renderer.setMasterGainQ15(masters[(block/128)%masters.size()]));
        std::array<std::int16_t,128> output{}; renderer.render(block*64,output.data());
        for(auto value:output) { hash^=static_cast<std::uint16_t>(value); hash*=UINT64_C(1099511628211); }
    }
    const auto& s=renderer.stats();
    require(s.clipped_samples>0 && s.events_rejected==0);
    pool.allSoundOff(); require(directory.stats().live_voice_refs==0);
    std::printf("OUTPUT_BYPASS_TRACE hash=%" PRIu64 " blocks=%" PRIu64 " frames=%" PRIu64
        " segments=%" PRIu64 " attempted=%" PRIu64 " applied=%" PRIu64 " rejected=%" PRIu64
        " clipped=%" PRIu64 " peak=%d\n",hash,s.blocks_rendered,s.output_frames,s.render_segments,
        s.events_attempted,s.events_applied,s.events_rejected,s.clipped_samples,s.peak_abs_accumulator);
}
