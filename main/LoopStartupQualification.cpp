#include "LoopStartupQualification.h"
#include "app/LoopStartupSession.h"
#include "loop_startup_manifest.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <new>

namespace gm_synth::bringup {
namespace {
using Session=app::LoopStartupSession<>;
namespace manifest=loop_startup_manifest;
constexpr unsigned internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
std::uint64_t hash_us=0;
app::StartupSerials serials; // boot-local owner, survives session reconstruction
struct Resources {
    esp_partition_mmap_handle_t mapping=0;
    bool mapped=false;
    void* arena=nullptr;
    unsigned releases=0;
    static void release(void* opaque) noexcept {
        auto& r=*static_cast<Resources*>(opaque);
        heap_caps_free(r.arena);r.arena=nullptr;
        if(r.mapped){esp_partition_munmap(r.mapping);r.mapped=false;}
        ++r.releases;
    }
};
bool hash(const std::uint8_t* p,std::size_t n,app::VerifiedReadOnlyBank::Digest& out) noexcept {
    const auto start=esp_timer_get_time();
    psa_hash_operation_t op=PSA_HASH_OPERATION_INIT;
    bool ok=psa_hash_setup(&op,PSA_ALG_SHA_256)==PSA_SUCCESS;
    for(std::size_t at=0;ok && at<n;at+=65536) {
        ok=psa_hash_update(&op,p+at,std::min<std::size_t>(65536,n-at))==PSA_SUCCESS;
        vTaskDelay(1); // verification never monopolizes the control task
    }
    std::size_t got=0;
    if(ok)ok=psa_hash_finish(&op,out.data(),out.size(),&got)==PSA_SUCCESS && got==out.size();
    (void)psa_hash_abort(&op);hash_us=esp_timer_get_time()-start;return ok;
}
bool checkpoint(void*) {vTaskDelay(1);return true;}
void memory(const char* phase) {
    std::printf("STARTUP_MEMORY phase=%s internal_free=%u internal_largest=%u internal_min=%u psram_free=%u\n",
        phase,unsigned(heap_caps_get_free_size(internal)),unsigned(heap_caps_get_largest_free_block(internal)),
        unsigned(heap_caps_get_minimum_free_size(internal)),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
realtime::RealtimeEvent event(Session& s,realtime::EventType type,unsigned a,unsigned b=0) {
    realtime::RealtimeEvent e{};e.channel=0;e.type=type;e.data1=a;e.data2=b;e.timestamp_frame=s.frame();return e;
}
} // namespace

void runLoopStartupQualification() {
    std::printf("STARTUP_BEGIN profile=%s analysis=%u root_application=1\n",manifest::profile,unsigned(manifest::analysis));
    memory("before");
    const auto* partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"loop_bank");
    if(!partition || !partition->readonly || manifest::bytes>partition->size || psa_crypto_init()!=PSA_SUCCESS) {
        std::puts("STARTUP_FAIL resource_preflight");return;
    }
    Resources r;const void* mapped=nullptr;
    // v6.1 write-blocking mapping + read-only partition; no OTA/write task is
    // started by this isolated profile. Not an arbitrary SD/live OTA lock.
    const auto flags=static_cast<esp_partition_mmap_flag_t>(ESP_PARTITION_MMAP_DATA|ESP_PARTITION_MMAP_BLOCKS_WRITE);
    if(esp_partition_mmap(partition,0,manifest::bytes,flags,&mapped,&r.mapping)!=ESP_OK) {
        std::puts("STARTUP_FAIL mmap");return;
    }
    r.mapped=true;
    const unsigned arena_caps=manifest::physical ? MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT : internal;
    r.arena=heap_caps_malloc(manifest::arena_bytes,arena_caps);
    if(!r.arena){Resources::release(&r);std::puts("STARTUP_FAIL arena_allocation");return;}
    app::StartupLease lease(static_cast<const std::uint8_t*>(mapped),manifest::bytes,
        r.arena,manifest::arena_bytes,&r,&Resources::release);
    void* object=heap_caps_malloc(sizeof(Session),internal);
    if(!object){std::puts("STARTUP_FAIL session_allocation");return;} // lease releases resources
    auto* session=new(object) Session;
    app::LoopStartupConfig config;config.program=manifest::initial_program;
    config.analysis.experimental_opt_in=manifest::analysis;config.analysis.checkpoint=checkpoint;
    const auto begin=esp_timer_get_time();
    const auto status=session->start(std::move(lease),manifest::digest,hash,serials,config);
    const auto startup_us=esp_timer_get_time()-begin;
    bool ok=status==app::LoopStartupStatus::Running;
    const auto initial=session->analysis();const auto initial_budget=session->budget();
    std::printf("STARTUP_READY status=%u session=%" PRIu64 " plans=%u analyzed=%u work=%u reads=%u arena_capacity=%u session_bytes=%u\n",
        unsigned(status),session->serial(),unsigned(session->stage().admitted),unsigned(initial.completed),unsigned(initial_budget.used),unsigned(initial_budget.pcm_reads),
        unsigned(session->residencyStats().capacity_bytes),unsigned(sizeof(Session)));
    std::printf("STARTUP_ASSET bytes=%u sha256=%s\n",unsigned(manifest::bytes),manifest::digest_hex);
    memory("ready");
    std::array<std::int16_t,128> output{};
    std::uint64_t trace=UINT64_C(14695981039346656037),render_total=0,render_max=0,service_max=0;
    unsigned blocks=0,initial_loops=0;std::uint16_t replacement_fade=0;
    if(ok)ok=session->enqueue(event(*session,realtime::EventType::NoteOn,60,100));
    for(unsigned i=0;ok && i<256;++i) {
        if(i==128)ok=session->enqueue(event(*session,realtime::EventType::ProgramChange,manifest::replacement_program));
        if(i==129)ok=session->enqueue(event(*session,realtime::EventType::NoteOn,60,100));
        const auto t=esp_timer_get_time();
        ok=ok && session->render(output.data());
        const std::uint64_t elapsed=esp_timer_get_time()-t;
        render_total+=elapsed;render_max=std::max(render_max,elapsed);++blocks;
        if(i==0)initial_loops=session->activeLoops();
        if(i<128 && session->budget().used!=initial_budget.used)ok=false;
        if(i==128 || i==129) {
            const auto c=esp_timer_get_time();const auto result=session->service();
            service_max=std::max(service_max,std::uint64_t(esp_timer_get_time()-c));
            ok=ok && (i==128 ? result==gm::ProgramChangeServiceStatus::Ok :
                result==gm::ProgramChangeServiceStatus::NoPending && session->activeProgram()==manifest::replacement_program);
            replacement_fade=session->analysis().analysis.selected_fade;
        }
        for(auto value:output){trace^=static_cast<std::uint16_t>(value);trace*=UINT64_C(1099511628211);}
        trace^=session->activeVoices();trace*=UINT64_C(1099511628211);
        if(i%16==0)vTaskDelay(1);
    }
    const auto audio=session->audioStats();const auto pcm=session->residencyStats();
    ok=ok && blocks==256 && audio.events_applied==3 && audio.events_rejected==0;
    if(!manifest::physical)ok=ok && initial_loops==(manifest::analysis?1u:0u) && replacement_fade==0 &&
        trace==(manifest::analysis?UINT64_C(2034267099694268986):UINT64_C(14037194901542623014));
    std::printf("STARTUP_RESULT frames=%" PRIu64 " trace=%" PRIu64 " initial_loops=%u replacement_fade=%u events=%" PRIu64 " rejected=%" PRIu64 "\n",
        audio.output_frames,trace,initial_loops,unsigned(replacement_fade),audio.events_applied,audio.events_rejected);
    std::printf("STARTUP_TIMING startup_us=%" PRIu64 " hash_us=%" PRIu64 " service_max_us=%" PRIu64 " render_avg_us=%" PRIu64 " render_max_us=%" PRIu64 " diagnostic_only=1\n",
        std::uint64_t(startup_us),hash_us,service_max,blocks?render_total/blocks:0,render_max);
    std::printf("STARTUP_PCM capacity=%u high_water=%u used=%u allocation_failures=%u\n",
        unsigned(pcm.capacity_bytes),unsigned(pcm.high_water_bytes),unsigned(pcm.used_bytes),unsigned(pcm.allocation_failures));
    ok=session->stop() && ok;
    session->~Session();heap_caps_free(object);
    ok=ok && r.releases==1 && !r.mapped && !r.arena;
    std::printf("STARTUP_STOP releases=%u mapped=%u arena_live=%u\n",r.releases,unsigned(r.mapped),unsigned(r.arena!=nullptr));
    memory("stopped");
    std::puts(ok?"STARTUP_PASS runtime_approved=0":"STARTUP_FAIL exercise");
    std::fflush(stdout);
}
} // namespace gm_synth::bringup

extern "C" void app_main(void) { gm_synth::bringup::runLoopStartupQualification(); }
