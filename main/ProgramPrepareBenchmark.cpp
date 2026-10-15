#include "ProgramPrepareBenchmark.h"

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "sample/ProgramPrepareController.h"
#include "sample/ProgramSnapshot.h"
#include "sample/PublishedSampleDirectory.h"
#include "sample/SampleResidencyManager.h"
#include "sf2/Sf2SampleSource.h"
#include "storage/VfsRandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::bringup {

#ifdef CONFIG_GM_SYNTH_PROGRAM_PREPARE_BENCHMARK_ENABLE
namespace {

constexpr char kTag[] = "prepare_bench";
constexpr std::size_t kMaxSamples = sample::SampleResidencyManager::kMaxResidentSamples;

struct DependencySet {
    std::array<sample::SampleId, kMaxSamples> ids{};
    std::size_t count = 0;
    std::size_t pcm_bytes = 0;
    std::size_t aligned_bytes = 0;
};

struct SourceFillContext {
    sf2::Sf2SampleSource* source = nullptr;
    sample::SampleId sample_id = 0;
};

bool fillFromSource(void* opaque, std::uint8_t* destination, std::size_t pcm_bytes) {
    auto& context = *static_cast<SourceFillContext*>(opaque);
    return context.source != nullptr &&
           context.source->fillSample(context.sample_id, destination, pcm_bytes);
}

bool setContains(const DependencySet& set, sample::SampleId id) {
    for (std::size_t i = 0; i < set.count; ++i) {
        if (set.ids[i] == id) {
            return true;
        }
    }
    return false;
}

bool buildDependencySet(
    sf2::Sf2SampleSource& source,
    bool reverse,
    const DependencySet* exclude,
    std::size_t target_aligned_bytes,
    std::size_t max_aligned_bytes,
    DependencySet* out) {
    if (out == nullptr || target_aligned_bytes == 0 || max_aligned_bytes < target_aligned_bytes) {
        return false;
    }

    *out = DependencySet{};
    const std::size_t sample_count = source.sampleCount();
    for (std::size_t ordinal = 0; ordinal < sample_count; ++ordinal) {
        const std::size_t raw_index = reverse ? (sample_count - 1u - ordinal) : ordinal;
        const auto id = static_cast<sample::SampleId>(raw_index);
        if (exclude != nullptr && setContains(*exclude, id)) {
            continue;
        }

        sample::SampleDescriptor descriptor{};
        if (!source.descriptorFor(id, &descriptor)) {
            return false;
        }
        const std::size_t aligned = sample::SampleResidencyManager::alignedBytes(descriptor.pcm_bytes);
        if (aligned > max_aligned_bytes) {
            continue;
        }
        if (out->count != 0 && out->aligned_bytes + aligned > max_aligned_bytes) {
            continue;
        }

        out->ids[out->count++] = id;
        out->pcm_bytes += descriptor.pcm_bytes;
        out->aligned_bytes += aligned;
        if (out->aligned_bytes >= target_aligned_bytes) {
            return true;
        }
    }

    return out->count != 0 && out->aligned_bytes >= target_aligned_bytes;
}

sample::ProgramPrepareStats statsDelta(
    const sample::ProgramPrepareStats& after,
    const sample::ProgramPrepareStats& before) {
    sample::ProgramPrepareStats out{};
#define GM_DELTA(field) out.field = after.field - before.field
    GM_DELTA(prepare_attempts);
    GM_DELTA(prepare_successes);
    GM_DELTA(prepare_failures);
    GM_DELTA(samples_loaded);
    GM_DELTA(sample_bytes_loaded);
    GM_DELTA(samples_reused_resident);
    GM_DELTA(source_failures);
    GM_DELTA(capacity_failures);
    GM_DELTA(capacity_retries);
    GM_DELTA(cache_evictions);
    GM_DELTA(cache_evicted_bytes);
    GM_DELTA(samples_pinned);
    GM_DELTA(samples_published);
    GM_DELTA(samples_reused_published);
    GM_DELTA(retire_acks);
    GM_DELTA(retired_pins);
    GM_DELTA(samples_unpublished_after_retire);
#undef GM_DELTA
    return out;
}

void logPrepareCase(
    const char* name,
    bool ok,
    std::int64_t elapsed_us,
    const DependencySet& set,
    const sample::ProgramPrepareStats& delta) {
    ESP_LOGI(
        kTag,
        "case=%s ok=%u elapsed_us=%lld dependencies=%u dependency_pcm_bytes=%u dependency_aligned_bytes=%u loaded=%llu loaded_bytes=%llu reused_resident=%llu evictions=%llu evicted_bytes=%llu capacity_retries=%llu",
        name,
        ok ? 1u : 0u,
        static_cast<long long>(elapsed_us),
        static_cast<unsigned>(set.count),
        static_cast<unsigned>(set.pcm_bytes),
        static_cast<unsigned>(set.aligned_bytes),
        static_cast<unsigned long long>(delta.samples_loaded),
        static_cast<unsigned long long>(delta.sample_bytes_loaded),
        static_cast<unsigned long long>(delta.samples_reused_resident),
        static_cast<unsigned long long>(delta.cache_evictions),
        static_cast<unsigned long long>(delta.cache_evicted_bytes),
        static_cast<unsigned long long>(delta.capacity_retries));
}

bool loadUnpinnedCacheSample(
    sf2::Sf2SampleSource& source,
    sample::SampleResidencyManager& residency,
    sample::SampleId id) {
    sample::SampleDescriptor descriptor{};
    if (!source.descriptorFor(id, &descriptor)) {
        return false;
    }

    SourceFillContext context{&source, id};
    return residency.loadFromSource(descriptor, fillFromSource, &context);
}

bool primeEvictableCache(
    sf2::Sf2SampleSource& source,
    sample::SampleResidencyManager& residency,
    const DependencySet& protected_a,
    const DependencySet& reserved_b,
    std::size_t* loaded_samples,
    std::size_t* loaded_bytes) {
    if (loaded_samples == nullptr || loaded_bytes == nullptr) {
        return false;
    }

    *loaded_samples = 0;
    *loaded_bytes = 0;
    for (std::size_t index = 0; index < source.sampleCount(); ++index) {
        const auto id = static_cast<sample::SampleId>(index);
        if (setContains(protected_a, id) || setContains(reserved_b, id)) {
            continue;
        }
        if (residency.isResident(id)) {
            continue;
        }

        sample::SampleDescriptor descriptor{};
        if (!source.descriptorFor(id, &descriptor)) {
            return false;
        }

        if (loadUnpinnedCacheSample(source, residency, id)) {
            ++(*loaded_samples);
            *loaded_bytes += descriptor.pcm_bytes;
            continue;
        }

        const auto error = residency.lastError();
        if (error == sample::ResidencyError::NoContiguousSpace ||
            error == sample::ResidencyError::MetadataFull) {
            continue;
        }
        return false;
    }
    return true;
}

bool runEnabledBenchmark(sample::SampleResidencyManager& residency) {
    using sample::ProgramPrepareController;
    using sample::ProgramSnapshotExchange;
    using sample::PublishedSampleDirectory;
    using sf2::Sf2SampleSource;
    using storage::VfsRandomAccessReader;

    const char* path = CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH;
    VfsRandomAccessReader reader;
    if (!reader.open(path)) {
        ESP_LOGE(kTag, "cannot open SF2 path: %s", path);
        return false;
    }

    static Sf2SampleSource source;
    if (!source.begin(reader)) {
        ESP_LOGE(kTag, "SF2 parse failed: error=%u", static_cast<unsigned>(source.lastError()));
        return false;
    }
    if (source.sampleCount() != 67u) {
        ESP_LOGE(
            kTag,
            "fixture sample-count mismatch: expected=67 actual=%u",
            static_cast<unsigned>(source.sampleCount()));
        return false;
    }

    static PublishedSampleDirectory directory;
    static ProgramSnapshotExchange exchange;
    static ProgramPrepareController controller(residency, directory, exchange, &source);
    controller.setSampleSource(&source);

    const auto initial_residency = residency.stats();
    const std::size_t arena_capacity = initial_residency.capacity_bytes;
    const std::size_t target_a = arena_capacity / 8u;
    const std::size_t target_b = arena_capacity / 6u;
    const std::size_t max_each = arena_capacity / 4u;

    DependencySet program_a{};
    DependencySet program_b{};
    if (!buildDependencySet(source, false, nullptr, target_a, max_each, &program_a) ||
        !buildDependencySet(source, true, &program_a, target_b, max_each, &program_b)) {
        ESP_LOGE(kTag, "failed to construct deterministic dependency sets");
        return false;
    }

    ESP_LOGI(
        kTag,
        "start path=%s samples=%u arena=%u A[count=%u aligned=%u] B[count=%u aligned=%u] read_chunk=%u",
        path,
        static_cast<unsigned>(source.sampleCount()),
        static_cast<unsigned>(arena_capacity),
        static_cast<unsigned>(program_a.count),
        static_cast<unsigned>(program_a.aligned_bytes),
        static_cast<unsigned>(program_b.count),
        static_cast<unsigned>(program_b.aligned_bytes),
        static_cast<unsigned>(Sf2SampleSource::kReadChunkBytes));

    auto before_stats = controller.stats();
    const std::int64_t cold_start = esp_timer_get_time();
    const bool cold_ok = controller.activateInitialProgram(
        0, 0, 0, 1001, program_a.ids.data(), program_a.count);
    const std::int64_t cold_elapsed = esp_timer_get_time() - cold_start;
    auto after_stats = controller.stats();
    auto delta = statsDelta(after_stats, before_stats);
    logPrepareCase("cold_initial", cold_ok, cold_elapsed, program_a, delta);
    if (!cold_ok || delta.samples_loaded == 0 || delta.sample_bytes_loaded == 0) {
        ESP_LOGE(kTag, "cold_initial did not exercise source-backed loading");
        return false;
    }

    before_stats = controller.stats();
    const std::int64_t warm_start = esp_timer_get_time();
    const bool warm_ok = controller.prepareProgram(
        0, 0, 0, 1002, program_a.ids.data(), program_a.count);
    const std::int64_t warm_elapsed = esp_timer_get_time() - warm_start;
    after_stats = controller.stats();
    delta = statsDelta(after_stats, before_stats);
    logPrepareCase("warm_same_program", warm_ok, warm_elapsed, program_a, delta);
    if (!warm_ok || delta.samples_loaded != 0 || delta.samples_reused_resident != program_a.count) {
        ESP_LOGE(kTag, "warm_same_program did not stay fully resident");
        return false;
    }

    const std::int64_t warm_commit_start = esp_timer_get_time();
    const bool warm_commit_ok = exchange.commitAtBlockBoundary(0);
    const std::int64_t warm_commit_us = esp_timer_get_time() - warm_commit_start;
    const std::int64_t warm_retire_start = esp_timer_get_time();
    const auto warm_ack = controller.acknowledgeAndRetire(0, 1002);
    const std::int64_t warm_retire_us = esp_timer_get_time() - warm_retire_start;
    ESP_LOGI(
        kTag,
        "case=warm_commit_retire commit_ok=%u ack_ok=%u commit_us=%lld retire_us=%lld",
        warm_commit_ok ? 1u : 0u,
        warm_ack.valid() ? 1u : 0u,
        static_cast<long long>(warm_commit_us),
        static_cast<long long>(warm_retire_us));
    if (!warm_commit_ok || !warm_ack.valid()) {
        return false;
    }

    std::size_t primed_samples = 0;
    std::size_t primed_bytes = 0;
    const std::int64_t prime_start = esp_timer_get_time();
    if (!primeEvictableCache(
            source,
            residency,
            program_a,
            program_b,
            &primed_samples,
            &primed_bytes)) {
        ESP_LOGE(kTag, "failed while priming evictable cache");
        return false;
    }
    const std::int64_t prime_elapsed = esp_timer_get_time() - prime_start;
    const auto primed_residency = residency.stats();
    ESP_LOGI(
        kTag,
        "case=prime_cache ok=1 elapsed_us=%lld loaded_samples=%u loaded_bytes=%u used=%u largest_free_span=%u free_spans=%u",
        static_cast<long long>(prime_elapsed),
        static_cast<unsigned>(primed_samples),
        static_cast<unsigned>(primed_bytes),
        static_cast<unsigned>(primed_residency.used_bytes),
        static_cast<unsigned>(primed_residency.largest_free_span_bytes),
        static_cast<unsigned>(primed_residency.free_span_count));

    before_stats = controller.stats();
    const std::int64_t eviction_start = esp_timer_get_time();
    const bool eviction_ok = controller.prepareProgram(
        0, 0, 1, 1003, program_b.ids.data(), program_b.count);
    const std::int64_t eviction_elapsed = esp_timer_get_time() - eviction_start;
    after_stats = controller.stats();
    delta = statsDelta(after_stats, before_stats);
    logPrepareCase("eviction_program_change", eviction_ok, eviction_elapsed, program_b, delta);
    if (!eviction_ok || delta.samples_loaded == 0 || delta.cache_evictions == 0) {
        ESP_LOGE(
            kTag,
            "eviction_program_change did not exercise source load + safe LRU eviction (loaded=%llu evictions=%llu)",
            static_cast<unsigned long long>(delta.samples_loaded),
            static_cast<unsigned long long>(delta.cache_evictions));
        return false;
    }

    auto release_tail = directory.acquire(program_a.ids[0]);
    if (!release_tail.valid()) {
        ESP_LOGE(kTag, "failed to acquire release-tail Voice Ref before commit");
        return false;
    }

    const std::int64_t change_commit_start = esp_timer_get_time();
    const bool change_commit_ok = exchange.commitAtBlockBoundary(0);
    const std::int64_t change_commit_us = esp_timer_get_time() - change_commit_start;
    const std::int64_t change_retire_start = esp_timer_get_time();
    const auto change_ack = controller.acknowledgeAndRetire(0, 1003);
    const std::int64_t change_retire_us = esp_timer_get_time() - change_retire_start;

    const bool tail_survived = release_tail.valid() &&
        release_tail.record != nullptr && release_tail.record->pcm != nullptr;
    const bool tail_release_ok = directory.release(release_tail);
    ESP_LOGI(
        kTag,
        "case=eviction_commit_retire commit_ok=%u ack_ok=%u commit_us=%lld retire_us=%lld release_tail_survived=%u release_ok=%u",
        change_commit_ok ? 1u : 0u,
        change_ack.valid() ? 1u : 0u,
        static_cast<long long>(change_commit_us),
        static_cast<long long>(change_retire_us),
        tail_survived ? 1u : 0u,
        tail_release_ok ? 1u : 0u);
    if (!change_commit_ok || !change_ack.valid() || !tail_survived || !tail_release_ok) {
        return false;
    }

    const auto final_residency = residency.stats();
    const auto final_prepare = controller.stats();
    ESP_LOGI(
        kTag,
        "PASS used=%u high_water=%u largest_free_span=%u resident_samples=%u total_prepare_successes=%llu total_loaded_bytes=%llu total_evictions=%llu total_evicted_bytes=%llu",
        static_cast<unsigned>(final_residency.used_bytes),
        static_cast<unsigned>(final_residency.high_water_bytes),
        static_cast<unsigned>(final_residency.largest_free_span_bytes),
        static_cast<unsigned>(final_residency.resident_sample_count),
        static_cast<unsigned long long>(final_prepare.prepare_successes),
        static_cast<unsigned long long>(final_prepare.sample_bytes_loaded),
        static_cast<unsigned long long>(final_prepare.cache_evictions),
        static_cast<unsigned long long>(final_prepare.cache_evicted_bytes));
    return true;
}

}  // namespace
#endif

bool runOptionalProgramPrepareBenchmark(sample::SampleResidencyManager& residency) {
#ifdef CONFIG_GM_SYNTH_PROGRAM_PREPARE_BENCHMARK_ENABLE
    return runEnabledBenchmark(residency);
#else
    (void)residency;
    return true;
#endif
}

}  // namespace gm_synth::bringup
