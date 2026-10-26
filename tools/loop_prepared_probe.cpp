// Host-only integration probe. I/O, heap and oversized host workspaces are NOT
// realtime code. Uses unmodified production Prepare/Source/Residency/Exchanges.
#include "voice/experimental/PreparedLoopHandoff.h"
#include "sample/ProgramPrepareController.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2SampleSource.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gm_synth;
using namespace gm_synth::voice::experimental;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::uint64_t number(const char* text) {
    const std::string s(text); std::uint64_t n=0;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),n);
    require(r.ec==std::errc{} && r.ptr==s.data()+s.size(),"invalid integer"); return n;
}
class FileReader final : public storage::RandomAccessReader {
public:
    explicit FileReader(const char* path) : f_(path,std::ios::binary|std::ios::ate) {
        require(bool(f_),"cannot open bank");
        const auto n=f_.tellg(); require(n>0,"empty bank"); bytes_=static_cast<std::uint64_t>(n);
    }
    std::uint64_t size() const override { return bytes_; }
    bool readAt(std::uint64_t offset,void* out,std::size_t n) override {
        if (!out || offset>bytes_ || n>bytes_-offset) return false;
        f_.clear(); f_.seekg(static_cast<std::streamoff>(offset));
        f_.read(static_cast<char*>(out),static_cast<std::streamsize>(n));
        return f_.gcount()==static_cast<std::streamsize>(n);
    }
private:
    std::ifstream f_; std::uint64_t bytes_=0;
};
PreparedLoopAsset asset(const char* hex) {
    const std::string s(hex); require(s.size()==64,"digest must be SHA256 hex");
    PreparedLoopAsset a; a.session=1; a.bank_generation=1;
    for (std::size_t i=0;i<32;++i) {
        unsigned v=0; auto r=std::from_chars(s.data()+2*i,s.data()+2*i+2,v,16);
        require(r.ec==std::errc{} && r.ptr==s.data()+2*i+2,"invalid digest");
        a.sha256[i]=static_cast<std::uint8_t>(v);
    }
    require(a.valid(),"empty asset identity"); return a;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==12,"usage: prepared_probe sf2 sha256 bank program zone fade step total block off release");
        const auto a=asset(argv[2]);
        const auto bank=number(argv[3]), program=number(argv[4]), zone=number(argv[5]);
        const auto fade=number(argv[6]), step=number(argv[7]), total=number(argv[8]);
        const auto block=number(argv[9]), off=number(argv[10]), release=number(argv[11]);
        require(bank<=65535 && program<=255 && zone<1024 &&
                (fade==0 || fade==16 || fade==32 || fade==64) && step && total && total<=4000000 &&
                block && block<=2048 && (release==0 || release==16 || release==32 || release==64) &&
                (!release || fade),"argument outside bounds");
        FileReader file(argv[1]); sf2::Sf2PdtaReader pdta; sf2::Sf2SampleSource source;
        require(pdta.begin(file) && source.begin(file),"source parsing failed");
        constexpr auto cap=gm::PlaybackProgramSnapshot::kMaxZones;
        std::vector<sf2::NativeResolvedZone> native(cap);
        std::vector<sf2::ResolvedZoneModulation> mods(cap);
        std::vector<gm::PlaybackZone> zones(cap);
        std::array<gm::PlaybackProgramKeySpan,128> spans{};
        std::vector<std::uint16_t> indices(cap*128);
        std::vector<sample::SampleId> dependencies(cap);
        gm::PlaybackProgramSnapshot snapshot;
        sf2::PlaybackPrepareRequest req;
        const std::uint8_t channel=bank==128?9:0;
        req.channel=channel; req.bank=bank; req.program=program; req.token=1; req.bank_generation=1;
        req.native_scratch=native.data(); req.native_capacity=native.size();
        req.modulation_scratch=mods.data(); req.modulation_capacity=mods.size();
        req.playback_zones=zones.data(); req.playback_capacity=zones.size();
        req.key_spans=spans.data(); req.key_zone_indices=indices.data(); req.key_zone_index_capacity=indices.size();
        req.dependencies=dependencies.data(); req.dependency_capacity=dependencies.size(); req.snapshot=&snapshot;
        sf2::Sf2PlaybackProgramPreparer preparer;
        const auto prepared=preparer.prepare(pdta,req);
        require(prepared.ok() && snapshot.valid() && zone<snapshot.zone_count,"program prepare failed");
        // Host arena is computed for this program's complete dependency set.
        // This gate measures semantics, NOT whether the program fits S3 PSRAM.
        std::size_t arena_bytes=4096;
        for (std::size_t i=0;i<prepared.dependency_count;++i) {
            sample::SampleDescriptor d;
            require(source.descriptorFor(dependencies[i],&d),"source dependency descriptor failed");
            arena_bytes+=sample::SampleResidencyManager::alignedBytes(d.pcm_bytes);
        }
        require(arena_bytes<=128u*1024u*1024u,"host arena safety cap exceeded");
        std::vector<std::uint32_t> arena((arena_bytes+3)/4);
        sample::SampleResidencyManager residency;
        require(residency.begin(arena.data(),arena.size()*4),"arena failed");
        sample::PublishedSampleDirectory directory; sample::ProgramSnapshotExchange resident_exchange;
        sample::ProgramPrepareController controller(residency,directory,resident_exchange,&source);
        gm::PlaybackProgramSnapshotExchange playback_exchange;
        require(controller.activateInitialProgram(channel,bank,program,1,dependencies.data(),prepared.dependency_count),"resident prepare failed");
        require(playback_exchange.setInitialActive(&snapshot),"playback activation failed");
        PreparedLoopContextBuilder builder;
        require(builder.begin(source,residency,directory,snapshot,*resident_exchange.active(channel),a)==PreparedLoopStatus::Ok,"builder rejected program");
        LoopPlanRequest proposal;
        require(builder.derive(zone,proposal.context)==PreparedLoopStatus::Ok,"builder rejected zone");
        proposal.experimental_opt_in=true;
        proposal.fade={fade!=0,static_cast<std::uint16_t>(fade)};
        proposal.release={release!=0,static_cast<std::uint16_t>(release)};
        const auto context=proposal.context;
        sf2::Sf2PdtaReader::SampleHeader sh;
        require(pdta.sampleHeader(context.sample_id,&sh),"sample header failed");
        PreparedLoopHandoff<> handoff(directory);
        require(handoff.controlBegin(builder)==PreparedLoopStatus::Ok,"handoff begin failed");
        auto stale=proposal; ++stale.context.sample_generation;
        require(handoff.controlAdd(builder,zone,stale)==PreparedLoopStatus::ProposalRejected,"stale proposal accepted");
        require(handoff.controlAdd(builder,zone,proposal)==PreparedLoopStatus::Ok,"handoff bind failed");
        require(handoff.controlPublish()==PreparedLoopStatus::Ok,"handoff publication failed");
        require(handoff.audioCommitAtBlockBoundary(playback_exchange.active(channel),resident_exchange.active(channel))==PreparedLoopStatus::Ok,"handoff commit failed");
        require(handoff.controlAcknowledge()==PreparedLoopStatus::Ok,"handoff acknowledgement failed");
        PinnedLoopPlayback voice;
        require(handoff.audioStart(zone,playback_exchange.active(channel),resident_exchange.active(channel),voice,step)==LoopBindingStatus::Ok,"playback start failed");
        const auto* old=directory.published(context.sample_id);
        require(old!=nullptr,"missing current publication");
        // Real program switch to an empty program: BOTH existing exchanges
        // commit, the sidecar retires, then control retires old dependency pins.
        gm::PlaybackProgramSnapshot empty{};
        empty.channel=channel; empty.bank=bank; empty.program=program; empty.token=2; empty.bank_generation=1;
        require(controller.prepareProgram(channel,bank,program,2,nullptr,0),"empty prepare failed");
        require(playback_exchange.publishReady(&empty),"empty playback READY failed");
        PreparedLoopContextBuilder next;
        require(next.begin(source,residency,directory,empty,*resident_exchange.ready(channel),a)==PreparedLoopStatus::Ok,"empty builder failed");
        require(handoff.controlBegin(next)==PreparedLoopStatus::Ok && handoff.controlPublish()==PreparedLoopStatus::Ok,"empty sidecar READY failed");
        require(resident_exchange.commitAtBlockBoundary(channel) && playback_exchange.commitAtBlockBoundary(channel),"program commit failed");
        require(handoff.audioCommitAtBlockBoundary(playback_exchange.active(channel),resident_exchange.active(channel))==PreparedLoopStatus::Ok,"empty sidecar commit failed");
        require(handoff.controlAcknowledge()==PreparedLoopStatus::Ok,"sidecar retire failed");
        require(playback_exchange.acknowledgeCommit(channel,2).valid(),"playback ACK failed");
        require(controller.acknowledgeAndRetire(channel,2).valid() && controller.lastError()==sample::ProgramPrepareError::None,"dependency retire failed");
        require(directory.published(context.sample_id)==nullptr && !directory.canReclaim(old),"old playback not pinned");
        require(residency.residentView(context.sample_id).pin_count==0,"old program pin not retired");
        std::vector<std::int16_t> pcm(block); std::vector<char> encoded(block*2);
        std::uint64_t done=0;
        while (done<total && voice.active()) {
            if (done==off) voice.noteOff();
            auto n=std::min(block,total-done); if (done<off) n=std::min(n,off-done);
            const auto got=voice.render(pcm.data(),n); require(got!=0,"no render progress");
            for (std::size_t i=0;i<got;++i) {
                const auto x=static_cast<std::uint16_t>(pcm[i]);
                encoded[2*i]=static_cast<char>(x&255); encoded[2*i+1]=static_cast<char>(x>>8);
            }
            std::cout.write(encoded.data(),static_cast<std::streamsize>(got*2)); done+=got;
        }
        require(voice.reset() && handoff.resetQuiescent(),"cleanup failed");
        require(directory.canReclaim(old) && residency.evict(context.sample_id),"last ref failed to reclaim");
        require(directory.stats().live_voice_refs==0 && directory.stats().stale_release_faults==0,"reference fault");
        require(std::cout.good(),"PCM write failed");
        const auto base=std::uint64_t{sh.start};
        std::cerr<<"{\"sample_id\":"<<context.sample_id<<",\"start\":"<<base+context.region.start
            <<",\"end\":"<<base+context.region.end<<",\"loop_start\":"<<base+context.region.loop_start
            <<",\"loop_end\":"<<base+context.region.loop_end<<",\"mode\":"<<static_cast<unsigned>(context.mode)
            <<",\"sample_generation\":"<<context.sample_generation<<",\"rate\":"<<context.sample_rate
            <<",\"frames\":"<<done<<",\"dependencies\":"<<prepared.dependency_count
            <<",\"host_arena_bytes\":"<<arena.size()*4<<",\"retired_before_render\":true,\"reclaimed\":true,\"live_refs\":0}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
