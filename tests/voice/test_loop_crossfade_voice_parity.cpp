#include "voice/VoiceRenderer.h"
#include "voice/experimental/LoopCrossfadeCursor.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace gm_synth;
using Cursor = voice::experimental::LoopCrossfadeCursor;
using Mode = voice::experimental::CursorLoopMode;

int main() {
    std::array<std::int16_t,587> pcm{};
    std::uint32_t rng=731;
    for (auto& x : pcm) { rng=rng*1664525u+1013904223u; x=static_cast<std::int16_t>(static_cast<int>(rng%65536)-32768); }
    unsigned cases=0;
    for (const auto mode : {voice::LoopMode::None,voice::LoopMode::Continuous,voice::LoopMode::UntilRelease})
        for (auto step : {Cursor::kOne/2,Cursor::kOne,Cursor::kOne+Cursor::kOne/4,3*Cursor::kOne+9})
            for (const bool region : {false,true}) for (unsigned off : {0u,219u,10000u}) {
                sample::PublishedSampleDirectory directory;
                sample::ResidentRecord record{};
                record.bank_generation=directory.bankGeneration();
                record.sample_generation=1;
                record.pcm=pcm.data();
                record.descriptor.id=0;
                record.descriptor.frames=pcm.size();
                record.descriptor.pcm_bytes=pcm.size()*sizeof(pcm[0]);
                record.descriptor.sample_rate=44100;
                record.descriptor.loop_start=19;
                record.descriptor.loop_end=531;
                assert(directory.publish(0,&record));
                voice::VoiceStartParams p{};
                p.sample_id=0; p.phase_increment_q32=step; p.loop_mode=mode;
                p.gain_left_q15=p.gain_right_q15=voice::kQ15Unity;
                p.envelope.delay_frames=p.envelope.attack_frames=p.envelope.hold_frames=p.envelope.decay_frames=0;
                p.envelope.sustain_q15=voice::kQ15Unity;
                p.envelope.release_frames=4096;
                if (region) {
                    p.sample_region.enabled=true;
                    p.sample_region.start_frame=11;
                    p.sample_region.end_frame=573;
                    p.sample_region.loop_start_frame=23;
                    p.sample_region.loop_end_frame=523;
                }
                voice::Voice v;
                Cursor cursor;
                voice::experimental::CursorRegion r{region?11u:0u,region?573u:587u,
                                                     region?23u:19u,region?523u:531u};
                const auto cmode=mode==voice::LoopMode::None ? Mode::None :
                    mode==voice::LoopMode::Continuous ? Mode::Continuous : Mode::UntilRelease;
                assert(v.start(directory,p));
                assert(cursor.start(pcm.data(),pcm.size(),r,cmode,step));
                for (unsigned i=0;i<1600;++i) {
                    if (i==off) { v.noteOff(); cursor.noteOff(); }
                    std::array<std::int32_t,2> actual{};
                    const auto vr=v.renderAdd(actual.data(),1);
                    std::int16_t raw=0;
                    const bool got=cursor.next(raw);
                    assert(vr.frames_rendered==(got?1u:0u));
                    std::int32_t gain=voice::kQ15Unity;
                    if (i>=off) gain=static_cast<std::int32_t>(std::int64_t{voice::kQ15Unity}*(4096-(i-off))/4096);
                    const std::int64_t product=std::int64_t{raw}*gain;
                    const auto expected=static_cast<std::int32_t>(product>=0 ? (product+16383)/32767 : -((-product+16383)/32767));
                    assert(actual[0]==expected && actual[1]==expected);
                }
                v.reset(); cursor.reset();
                assert(directory.stats().live_voice_refs==0);
                ++cases;
            }
    std::cout << "Production Voice Direct parity: " << cases << " cases PASS\n";
}
