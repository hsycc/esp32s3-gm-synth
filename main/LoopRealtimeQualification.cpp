#include "LoopRealtimeQualification.h"
#include "LoopRealtimeI2s.h"
#include "app/LoopRealtimeSession.h"
#include "realtime/RealtimeHealthTracker.h"
#include "loop_realtime_manifest.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace gm_synth::bringup {
namespace {
using Session = app::LoopRealtimeSession<>;
using Storage = Session::Storage;
namespace manifest = loop_realtime_manifest;
constexpr unsigned kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr unsigned kBudgetUs = 1451;
constexpr unsigned kCohortBlocks = 4096;
app::StartupSerials serials;
std::uint64_t hash_us = 0;

struct Resources {
    esp_partition_mmap_handle_t mapping = 0;
    bool mapped = false;
    void* arena = nullptr;
    unsigned releases = 0;
    static void release(void* opaque) noexcept {
        auto& self = *static_cast<Resources*>(opaque);
        heap_caps_free(self.arena); self.arena = nullptr;
        if (self.mapped) { esp_partition_munmap(self.mapping); self.mapped = false; }
        ++self.releases;
    }
};

// Allocation-free 16-us buckets. Quantiles are explicitly upper bounds, not
// falsely precise p95/p99 values. The last bucket's bound is the observed max.
struct Latency {
    std::array<std::uint32_t, 256> buckets{};
    std::uint64_t total = 0;
    std::uint32_t count = 0, maximum = 0;
    void add(std::uint32_t value) {
        ++buckets[std::min<std::uint32_t>(value / 16u, 255u)];
        ++count; total += value; maximum = std::max(maximum, value);
    }
    std::uint32_t average() const { return count ? static_cast<std::uint32_t>(total / count) : 0; }
    std::uint32_t upper(unsigned percent) const {
        const auto rank = (std::uint64_t{count} * percent + 99u) / 100u;
        std::uint64_t sum = 0;
        if (!count) return 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            sum += buckets[i];
            if (sum >= rank) return i == 255u ? maximum : static_cast<std::uint32_t>((i + 1u) * 16u - 1u);
        }
        return maximum;
    }
};

struct Context {
    Session* session = nullptr;
    TaskHandle_t parent = nullptr;
    std::atomic<std::uint32_t> audio_done{0}, control_done{0}, audio_ready{0};
    std::atomic<std::uint32_t> frame{0}, program{manifest::initial_program};
    std::atomic<std::uint32_t> cohort_first{0}, cohort_end{0};
    // Each following member is owned by ONE actor, read by coordinator after join.
    bool audio_ok = true, control_ok = true;
    int audio_core = -1, control_core = -1;
    unsigned blocks = 0, voices_peak = 0, loops_peak = 0, program_switches = 0;
    unsigned blocks_during_prepare = 0, audio_stack = 0, control_stack = 0;
    unsigned queued = 0, note_ons = 0, note_offs = 0, cc = 0, sustain = 0, pc = 0;
    unsigned io_yields = 0, analysis_yields = 0, service_calls = 0;
    unsigned stalls = 0, work_consecutive = 0;
    unsigned cohort_blocks = 0, cohort_voices_min = 32, cohort_voices_max = 0;
    unsigned cohort_loops_min = 32, cohort_loops_max = 0;
    std::uint64_t cohort_clipped = 0;
    std::uint64_t checksum = UINT64_C(14695981039346656037);
    Latency render, service, cohort_render;
    realtime::RealtimeHealthTracker health{kBudgetUs, LoopRealtimeI2s::kDmaWindowUs};
    realtime::RealtimeHealthTracker cohort_health{kBudgetUs, LoopRealtimeI2s::kDmaWindowUs};
    LoopRealtimeI2sStats i2s{};
    Context() {
        for (auto* word : {&audio_done, &control_done, &audio_ready, &frame, &program, &cohort_first, &cohort_end})
            util::requireRealtimeAtomic32(*word);
    }
};

std::uint32_t elapsed(std::int64_t start, std::int64_t end) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end > start ? static_cast<std::uint64_t>(end - start) : 0,
        std::numeric_limits<std::uint32_t>::max()));
}
bool hash(const std::uint8_t* data, std::size_t bytes, app::VerifiedReadOnlyBank::Digest& out) noexcept {
    const auto before = esp_timer_get_time();
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (std::size_t at = 0; ok && at < bytes; at += 65536u) {
        ok = psa_hash_update(&operation, data + at, std::min<std::size_t>(65536u, bytes - at)) == PSA_SUCCESS;
        vTaskDelay(1);
    }
    std::size_t written = 0;
    if (ok) ok = psa_hash_finish(&operation, out.data(), out.size(), &written) == PSA_SUCCESS && written == out.size();
    (void)psa_hash_abort(&operation);
    hash_us = esp_timer_get_time() - before;
    return ok;
}
bool ioCheckpoint(void* opaque) {
    auto& context = *static_cast<Context*>(opaque);
    ++context.io_yields; vTaskDelay(1);
    return !context.session->stopRequested();
}
bool analysisCheckpoint(void* opaque) {
    auto& context = *static_cast<Context*>(opaque);
    ++context.analysis_yields; vTaskDelay(1);
    return !context.session->stopRequested();
}
void memory(const char* phase) {
    std::printf("REALTIME_MEMORY phase=%s internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_largest=%u\n",
        phase, unsigned(heap_caps_get_free_size(kInternal)), unsigned(heap_caps_get_largest_free_block(kInternal)),
        unsigned(heap_caps_get_minimum_free_size(kInternal)), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

bool enqueue(Context& c, std::uint32_t frame, realtime::EventType type, unsigned a, unsigned b = 0) {
    realtime::RealtimeEvent event{};
    event.timestamp_frame = frame; event.channel = 0; event.type = type;
    event.data1 = static_cast<std::uint8_t>(a); event.data2 = static_cast<std::uint8_t>(b);
    if (!c.session->enqueue(event)) return false;
    ++c.queued;
    switch (type) {
    case realtime::EventType::NoteOn: ++c.note_ons; break;
    case realtime::EventType::NoteOff: ++c.note_offs; break;
    case realtime::EventType::ControlChange: ++c.cc; if (a == 64) ++c.sustain; break;
    case realtime::EventType::ProgramChange: ++c.pc; break;
    default: break;
    }
    return true;
}
bool initialEvents(Context& c) {
    using T = realtime::EventType;
    return enqueue(c, 0, T::ControlChange, 91, 40) && enqueue(c, 0, T::ControlChange, 93, 20) &&
        enqueue(c, 0, T::ControlChange, 21, 0) && enqueue(c, 0, T::NoteOn, 60, 100) &&
        enqueue(c, 0, T::NoteOn, 61, 100) && enqueue(c, 32u * 64u, T::ControlChange, 64, 127) &&
        enqueue(c, 48u * 64u + 7u, T::NoteOff, 61) && enqueue(c, 80u * 64u, T::ControlChange, 21, 64) &&
        enqueue(c, 96u * 64u, T::ControlChange, 64, 0) && enqueue(c, 112u * 64u, T::ControlChange, 21, 127) &&
        enqueue(c, 128u * 64u, T::ProgramChange, manifest::replacement_program);
}
bool replacementEvents(Context& c, std::uint32_t frame) {
    using T = realtime::EventType;
    const auto base = frame + 16u * 64u;
    return enqueue(c, base, T::NoteOn, 60, 100) &&
        enqueue(c, base + 32u * 64u + 11u, T::NoteOff, 60) &&
        enqueue(c, base + 64u * 64u, T::ProgramChange, manifest::initial_program);
}
bool returnedEvents(Context& c, std::uint32_t frame) {
    using T = realtime::EventType;
    const auto base = frame + 16u * 64u;
    bool ok = enqueue(c, base, T::NoteOn, 61, 100) &&
        enqueue(c, base + 16u * 64u, T::ControlChange, 64, 127) &&
        enqueue(c, base + 32u * 64u + 3u, T::NoteOff, 61) &&
        enqueue(c, base + 64u * 64u, T::ControlChange, 64, 0) &&
        enqueue(c, base + 80u * 64u, T::ControlChange, 21, 0);
    if (!ok || !manifest::physical || manifest::synthetic) return ok;
    // One actual 24-sample-Voice full-FX regression cohort. This is not a sweep
    // or an assertion that the previous M8 stable boundary transfers unchanged.
    const auto begin = base + 128u * 64u;
    const auto first = begin + 64u; // onset burst remains in global statistics
    const auto end = first + kCohortBlocks * 64u;
    if (end + 128u * 64u >= manifest::blocks * 64u) return false;
    ok = enqueue(c, begin - 64u, T::ControlChange, 120, 0);
    for (unsigned key = 48; ok && key < 72; ++key) ok = enqueue(c, begin, T::NoteOn, key, 64);
    for (unsigned key = 48; ok && key < 72; ++key) ok = enqueue(c, end, T::NoteOff, key);
    if (ok) {
        c.cohort_end.store(end, std::memory_order_relaxed);
        c.cohort_first.store(first, std::memory_order_release);
    }
    return ok;
}

void controlTask(void* opaque) {
    auto& c = *static_cast<Context*>(opaque);
    auto& s = *c.session;
    c.control_core = xPortGetCoreID();
    while (!c.audio_ready.load(std::memory_order_acquire) && !s.stopRequested()) vTaskDelay(1);
    unsigned stage = 0;
    while (!s.stopRequested()) {
        const auto before = esp_timer_get_time();
        const auto result = s.controlService();
        const auto duration = elapsed(before, esp_timer_get_time());
        ++c.service_calls;
        if (result == gm::ProgramChangeServiceStatus::Ok) c.service.add(duration);
        else if (result != gm::ProgramChangeServiceStatus::NoPending &&
                 result != gm::ProgramChangeServiceStatus::TransitionBusy) {
            c.control_ok = false; s.requestStop(); break;
        }
        const auto program = c.program.load(std::memory_order_acquire);
        const auto frame = c.frame.load(std::memory_order_acquire);
        if (stage == 0 && program == manifest::replacement_program) {
            c.control_ok = replacementEvents(c, frame); stage = 1;
        } else if (stage == 1 && program == manifest::initial_program &&
                   s.controlServiceStats().service_successes >= 2) {
            c.control_ok = returnedEvents(c, frame); stage = 2;
        }
        if (!c.control_ok) { s.requestStop(); break; }
        vTaskDelay(1); // give CPU0 idle its normal WDT opportunity
    }
    c.control_ok = c.control_ok && stage == 2;
    c.control_stack = uxTaskGetStackHighWaterMark(nullptr);
    s.controlStop();
    const auto parent = c.parent;
    c.control_done.store(1, std::memory_order_release);
    xTaskNotifyGive(parent); // no later access to Context or Session
    vTaskDelete(nullptr);
}

void audioTask(void* opaque) {
    auto& c = *static_cast<Context*>(opaque);
    auto& s = *c.session;
    c.audio_core = xPortGetCoreID();
    LoopRealtimeI2s sink;
    c.audio_ok = !manifest::physical || sink.start();
    c.audio_ready.store(1, std::memory_order_release);
    std::array<std::int16_t, 128> output{};
    unsigned previous_program = manifest::initial_program;
    std::int64_t previous_write_end = 0;
    std::uint64_t previous_clipped = 0;
    for (unsigned block = 0; c.audio_ok && block < manifest::blocks && !s.stopRequested(); ++block) {
        const auto cycle_start = esp_timer_get_time();
        // Account for the preceding block's bounded post-write diagnostics as
        // producer work too; they are not free CPU time between DMA writes.
        const auto carried_work_us = previous_write_end ? elapsed(previous_write_end, cycle_start) : 0u;
        const auto phase = s.phase();
        c.audio_ok = s.audioRender(output.data());
        const auto render_end = esp_timer_get_time();
        if (!c.audio_ok) break;
        const auto render_us = elapsed(cycle_start, render_end);
        c.render.add(render_us); ++c.blocks;
        const auto program = s.audioProgram();
        const auto voices = s.audioVoices();
        const auto loops = s.audioLoops();
        c.voices_peak = std::max(c.voices_peak, voices);
        c.loops_peak = std::max(c.loops_peak, loops);
        if (program != previous_program) { ++c.program_switches; previous_program = program; }
        if (phase == Session::Phase::Preparing || s.phase() == Session::Phase::Preparing) ++c.blocks_during_prepare;
        for (auto value : output) { c.checksum ^= static_cast<std::uint16_t>(value); c.checksum *= UINT64_C(1099511628211); }
        const auto frame = static_cast<std::uint32_t>(s.audioFrame());
        c.frame.store(frame, std::memory_order_release);
        c.program.store(program, std::memory_order_release);
        std::uint32_t downmix_us = 0;
        const auto write_start = esp_timer_get_time();
        if (manifest::physical) c.audio_ok = sink.write(output.data(), downmix_us);
        else vTaskDelay(1); // only emulated pacing; never physical timing evidence
        const auto cycle_end = esp_timer_get_time();
        previous_write_end = cycle_end;
        const auto work_us = elapsed(cycle_start, write_start) + downmix_us + carried_work_us;
        c.health.addBlock(render_us, work_us);
        const auto first = c.cohort_first.load(std::memory_order_acquire);
        const auto end = c.cohort_end.load(std::memory_order_relaxed);
        const auto clipped_now = s.audioStats().clipped_samples;
        if (first && frame - 64u >= first && frame - 64u < end) {
            ++c.cohort_blocks; c.cohort_render.add(render_us); c.cohort_health.addBlock(render_us, work_us);
            c.cohort_voices_min = std::min(c.cohort_voices_min, voices);
            c.cohort_voices_max = std::max(c.cohort_voices_max, voices);
            c.cohort_loops_min = std::min(c.cohort_loops_min, loops);
            c.cohort_loops_max = std::max(c.cohort_loops_max, loops);
            c.cohort_clipped += clipped_now - previous_clipped;
        }
        previous_clipped = clipped_now;
        if (manifest::physical) {
            if (elapsed(cycle_start, cycle_end) + carried_work_us > LoopRealtimeI2s::kDmaWindowUs) ++c.stalls;
            c.work_consecutive = work_us > kBudgetUs ? c.work_consecutive + 1u : 0;
            // A real overload is a recorded failure, not a reason to change the
            // load/buffer/WDT. End before sustained CPU1 idle starvation.
            if (c.work_consecutive >= 128u || c.health.snapshot().backlog_exceeded_dma_window) c.audio_ok = false;
        }
    }
    c.audio_stack = uxTaskGetStackHighWaterMark(nullptr);
    c.i2s = sink.stats();
    s.audioStop();
    sink.stop();
    const auto parent = c.parent;
    c.audio_done.store(1, std::memory_order_release);
    xTaskNotifyGive(parent); // no later access to Context or Session
    vTaskDelete(nullptr);
}

bool join(Context& c) {
    const auto deadline = esp_timer_get_time() + INT64_C(120000000);
    while ((!c.audio_done.load(std::memory_order_acquire) || !c.control_done.load(std::memory_order_acquire)) &&
           esp_timer_get_time() < deadline) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (c.audio_done.load(std::memory_order_acquire) && c.control_done.load(std::memory_order_acquire)) return true;
    c.session->requestStop();
    const auto stop_deadline = esp_timer_get_time() + INT64_C(10000000);
    while ((!c.audio_done.load(std::memory_order_acquire) || !c.control_done.load(std::memory_order_acquire)) &&
           esp_timer_get_time() < stop_deadline) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (!c.audio_done.load(std::memory_order_acquire) || !c.control_done.load(std::memory_order_acquire)) {
        // Never destroy/unmap an object another task might still reference.
        std::puts("REALTIME_FAIL task_join_timeout resources_retained=1"); std::fflush(stdout); std::abort();
    }
    return false;
}

} // namespace

void runLoopRealtimeQualification() {
    std::printf("REALTIME_BEGIN profile=%s analysis=%u synthetic=%u root_application=1 control_core=0 audio_core=1\n",
        manifest::profile, unsigned(manifest::analysis), unsigned(manifest::synthetic));
    memory("before");
    const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "loop_bank");
    if (!partition || !partition->readonly || manifest::bytes > partition->size || psa_crypto_init() != PSA_SUCCESS) {
        std::puts("REALTIME_FAIL resource_preflight"); return;
    }
    Resources resources;
    const void* mapped = nullptr;
    const auto flags = static_cast<esp_partition_mmap_flag_t>(ESP_PARTITION_MMAP_DATA | ESP_PARTITION_MMAP_BLOCKS_WRITE);
    if (esp_partition_mmap(partition, 0, manifest::bytes, flags, &mapped, &resources.mapping) != ESP_OK) {
        std::puts("REALTIME_FAIL mmap"); return;
    }
    resources.mapped = true;
    const auto storage_caps = manifest::physical ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : kInternal;
    resources.arena = heap_caps_malloc(manifest::arena_bytes, storage_caps);
    if (!resources.arena) { Resources::release(&resources); std::puts("REALTIME_FAIL arena_allocation"); return; }
    app::StartupLease lease(static_cast<const std::uint8_t*>(mapped), manifest::bytes,
        resources.arena, manifest::arena_bytes, &resources, &Resources::release);
    void* storage_memory = heap_caps_malloc(sizeof(Storage), storage_caps);
    void* session_memory = heap_caps_malloc(sizeof(Session), kInternal);
    void* context_memory = heap_caps_malloc(sizeof(Context), kInternal);
    if (!storage_memory || !session_memory || !context_memory) {
        heap_caps_free(context_memory); heap_caps_free(session_memory); heap_caps_free(storage_memory);
        std::puts("REALTIME_FAIL metadata_allocation"); return;
    }
    auto* storage = new(storage_memory) Storage;
    auto* session = new(session_memory) Session(*storage);
    auto* c = new(context_memory) Context;
    c->session = session; c->parent = xTaskGetCurrentTaskHandle();
    app::LoopRealtimeConfig config;
    config.program = manifest::initial_program;
    config.analysis.experimental_opt_in = manifest::analysis;
    config.analysis.checkpoint = analysisCheckpoint; config.analysis.checkpoint_context = c;
    config.io_checkpoint = ioCheckpoint; config.io_context = c;
    config.now_us = []() -> std::uint64_t { return static_cast<std::uint64_t>(esp_timer_get_time()); };
    const auto before = esp_timer_get_time();
    const auto status = session->start(std::move(lease), manifest::digest, hash, serials, config);
    const auto startup_us = static_cast<std::uint64_t>(esp_timer_get_time() - before);
    bool ok = status == app::LoopStartupStatus::Running;
    std::printf("REALTIME_READY status=%u session=%" PRIu64 " plans=%u analyses=%u session_bytes=%u storage_bytes=%u storage_psram=%u arena_capacity=%u\n",
        unsigned(status), session->serial(), unsigned(session->controlStage().admitted),
        unsigned(session->controlAnalysis().completed), unsigned(sizeof(Session)), unsigned(sizeof(Storage)),
        unsigned(manifest::physical), unsigned(session->controlResidencyStats().capacity_bytes));
    std::printf("REALTIME_ASSET bytes=%u sha256=%s initial_program=%u replacement_program=%u\n",
        unsigned(manifest::bytes), manifest::digest_hex, unsigned(manifest::initial_program), unsigned(manifest::replacement_program));
    memory("ready");
    ok = ok && initialEvents(*c) && session->beginActors();
    bool started = false;
    if (ok) {
        started = true;
        const auto control_created = xTaskCreatePinnedToCore(controlTask, "loop-control", 16384, c, 5, nullptr, 0);
        if (control_created != pdPASS) {
            c->control_ok = false; session->controlStop(); c->control_done.store(1, std::memory_order_release);
        }
        const auto audio_created = control_created == pdPASS ?
            xTaskCreatePinnedToCore(audioTask, "loop-audio", CONFIG_GM_SYNTH_I2S_QUAL_TASK_STACK_BYTES,
                c, CONFIG_GM_SYNTH_I2S_QUAL_TASK_PRIORITY, nullptr, 1) : pdFAIL;
        if (audio_created != pdPASS) {
            c->audio_ok = false; session->audioStop(); c->audio_done.store(1, std::memory_order_release);
        }
        ok = join(*c) && control_created == pdPASS && audio_created == pdPASS && ok;
    }
    const auto audio = session->audioStats();
    const auto service = session->controlServiceStats();
    const auto io = session->controlIoStats();
    const auto analysis_timing = session->controlAnalysisTiming();
    const auto pcm = session->controlResidencyStats();
    const auto queue = session->queueStats();
    const auto publication = session->publicationStats();
    const auto health = c->health.snapshot();
    const auto cohort = c->cohort_health.snapshot();
    const bool cohort_required = manifest::physical && !manifest::synthetic;
    const bool cohort_ok = !cohort_required || (c->cohort_blocks == kCohortBlocks &&
        c->cohort_voices_min == 24 && c->cohort_voices_max == 24 &&
        (manifest::analysis ? c->cohort_loops_min == 24 : c->cohort_loops_max == 0) &&
        c->cohort_clipped == 0 && cohort.backlogHealthy() && c->cohort_render.maximum <= kBudgetUs);
    const bool physical_io_ok = !manifest::physical || (health.backlogHealthy() && c->stalls == 0 &&
        c->i2s.sent > 0 && c->i2s.queue_overflows == 0 && c->i2s.write_errors == 0 &&
        c->i2s.timeouts == 0 && c->i2s.partial_writes == 0);
    const bool integration_ok = ok && c->audio_ok && c->control_ok && c->blocks == manifest::blocks &&
        audio.events_applied == c->queued && audio.events_rejected == 0 &&
        c->program_switches == 2 && service.service_successes == 2 && service.ack_failures == 0 &&
        (manifest::analysis ? c->loops_peak > 0 && c->blocks_during_prepare > 0 : c->loops_peak == 0) &&
        queue.rejected_full == 0 && queue.rejected_invalid == 0 && queue.rejected_out_of_order == 0 &&
        publication.stale_release_faults == 0 && c->audio_stack >= 2048 && c->control_stack >= 2048 &&
        c->control_core == 0 && c->audio_core == 1 && physical_io_ok;
    // Transaction/I2S integration, measured cohort and digital headroom remain
    // separate results. Any failed required gate keeps the overall result FAIL.
    ok = integration_ok && cohort_ok && audio.clipped_samples == 0;
    std::printf("REALTIME_ACTORS control_core=%d audio_core=%d\n", c->control_core, c->audio_core);
    std::printf("REALTIME_GATES integration=%s cohort=%s digital_headroom=%s\n",
        integration_ok ? "PASS" : "FAIL", cohort_required ? (cohort_ok ? "PASS" : "FAIL") : "NOT_RUN",
        audio.clipped_samples == 0 ? "PASS" : "FAIL");
    std::printf("REALTIME_RESULT frames=%" PRIu64 " blocks=%u checksum=%" PRIu64 " events=%" PRIu64 " rejected=%" PRIu64 " clipped=%" PRIu64 " voices_peak=%u loop_voices_peak=%u program_switches=%u blocks_during_prepare=%u\n",
        audio.output_frames, c->blocks, c->checksum, audio.events_applied, audio.events_rejected,
        audio.clipped_samples, c->voices_peak, c->loops_peak, c->program_switches, c->blocks_during_prepare);
    std::printf("REALTIME_CONTROL queued=%u note_on=%u note_off=%u cc=%u sustain=%u program_change=%u services=%u prepared=%" PRIu64 " acknowledgements=%" PRIu64 " ack_failures=%" PRIu64 " io_reads=%" PRIu64 " io_bytes=%" PRIu64 " io_yields=%u analysis_yields=%u\n",
        c->queued, c->note_ons, c->note_offs, c->cc, c->sustain, c->pc, c->service_calls,
        service.service_successes, service.commit_acks, service.ack_failures, io.reads, io.bytes, c->io_yields, c->analysis_yields);
    std::printf("REALTIME_TIMING startup_us=%" PRIu64 " hash_us=%" PRIu64 " prepare_avg_us=%u prepare_max_us=%u render_avg_us=%u render_p95_upper_us=%u render_p99_upper_us=%u render_max_us=%u quantile_bucket_us=16 physical=%u\n",
        startup_us, hash_us, unsigned(c->service.average()), unsigned(c->service.maximum),
        unsigned(c->render.average()), unsigned(c->render.upper(95)),
        unsigned(c->render.upper(99)), unsigned(c->render.maximum), unsigned(manifest::physical));
    std::printf("REALTIME_STAGES read_us=%" PRIu64 " read_max_us=%" PRIu64 " analysis_us=%" PRIu64 " analysis_max_us=%" PRIu64 " analysis_calls=%" PRIu64 " read_excludes_yield=1 analysis_includes_yield=1\n",
        io.read_total_us, io.read_max_us, analysis_timing.total_us, analysis_timing.max_us, analysis_timing.calls);
    std::printf("REALTIME_HEALTH render_soft_overruns=%u work_soft_overruns=%u max_backlog_us=%" PRIu64 " final_backlog_us=%" PRIu64 " min_dma_margin_us=%u exceeded=%u stalls=%u writes=%u write_errors=%u timeouts=%u partial=%u sent=%u dma_queue_overflows=%u event_queue_drops=%u audio_stack=%u control_stack=%u\n",
        unsigned(health.render_soft_overruns), unsigned(health.producer_work_soft_overruns), health.max_backlog_us, health.final_backlog_us,
        unsigned(health.min_dma_margin_us), unsigned(health.backlog_exceeded_dma_window), c->stalls, unsigned(c->i2s.writes),
        unsigned(c->i2s.write_errors), unsigned(c->i2s.timeouts), unsigned(c->i2s.partial_writes), unsigned(c->i2s.sent), unsigned(c->i2s.queue_overflows),
        unsigned(queue.rejected_full + queue.rejected_invalid + queue.rejected_out_of_order), c->audio_stack, c->control_stack);
    std::printf("REALTIME_COHORT requested=24 required=%u blocks=%u voices_min=%u voices_max=%u loops_min=%u loops_max=%u clipped=%" PRIu64 " render_avg_us=%u render_p95_upper_us=%u render_p99_upper_us=%u render_max_us=%u max_backlog_us=%" PRIu64 " final_backlog_us=%" PRIu64 " gate=%s\n",
        unsigned(cohort_required), c->cohort_blocks, c->cohort_blocks ? c->cohort_voices_min : 0, c->cohort_voices_max,
        c->cohort_blocks ? c->cohort_loops_min : 0, c->cohort_loops_max, c->cohort_clipped, unsigned(c->cohort_render.average()),
        unsigned(c->cohort_render.upper(95)), unsigned(c->cohort_render.upper(99)), unsigned(c->cohort_render.maximum),
        cohort.max_backlog_us, cohort.final_backlog_us, cohort_required ? (cohort_ok ? "PASS" : "FAIL") : "NOT_RUN");
    std::printf("REALTIME_PCM capacity=%u high_water=%u used=%u allocation_failures=%u stale_refs=%" PRIu64 "\n",
        unsigned(pcm.capacity_bytes), unsigned(pcm.high_water_bytes), unsigned(pcm.used_bytes), unsigned(pcm.allocation_failures),
        publication.stale_release_faults);
    memory("joined");
    const bool audio_stopped = session->audioStopped(), control_stopped = session->controlStopped();
    ok = session->stopQuiescent() && ok;
    const auto final_publication = session->publicationStats();
    ok = ok && final_publication.live_voice_refs == 0 && final_publication.active_acquirers == 0 &&
        final_publication.stale_release_faults == 0;
    session->~Session(); storage->~Storage(); c->~Context();
    heap_caps_free(context_memory); heap_caps_free(session_memory); heap_caps_free(storage_memory);
    ok = ok && resources.releases == 1 && !resources.mapped && !resources.arena;
    std::printf("REALTIME_STOP started=%u audio_stopped=%u control_stopped=%u releases=%u mapped=%u arena_live=%u live_refs=%u acquirers=%u stale_refs=%" PRIu64 "\n",
        unsigned(started), unsigned(audio_stopped), unsigned(control_stopped), resources.releases,
        unsigned(resources.mapped), unsigned(resources.arena != nullptr), unsigned(final_publication.live_voice_refs),
        unsigned(final_publication.active_acquirers), final_publication.stale_release_faults);
    memory("stopped");
    std::puts(ok ? "REALTIME_PASS runtime_approved=0 physical_timing_approved=0" : "REALTIME_FAIL exercise");
    std::fflush(stdout);
}
} // namespace gm_synth::bringup

extern "C" void app_main(void) { gm_synth::bringup::runLoopRealtimeQualification(); }
