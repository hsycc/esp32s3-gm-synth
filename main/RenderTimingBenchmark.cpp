#include "RenderTimingBenchmark.h"

#include "sdkconfig.h"

#ifdef CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform/EspRealtimeClock.h"
#include "realtime/BlockEventScheduler.h"
#include "realtime/RealtimeEventQueue.h"
#include "realtime/RenderDeadlineMonitor.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "realtime/TimedAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/AdaptivePolyphony.h"
#include "voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr sample::SampleId kBenchmarkSampleId = 0;
constexpr std::size_t kPcmFrames = 1024;
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kWarmupBlocks = 16;
constexpr std::size_t kMeasuredBlocks = realtime::RenderDeadlineMonitor::kWindowSize;
constexpr std::array<std::size_t, 8> kVoiceCounts{{1, 4, 8, 12, 16, 18, 20, 24}};
constexpr std::size_t kDiagnosticVoices = 16;
constexpr std::int16_t kDiagnosticReverbSendUnits = 500;
constexpr std::int16_t kDiagnosticChorusSendUnits = 500;

#if defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
constexpr char kCompilerOptimization[] = "perf";
constexpr bool kPerformanceOptimizedBuild = true;
#else
constexpr char kCompilerOptimization[] = "non_perf";
constexpr bool kPerformanceOptimizedBuild = false;
#endif

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
constexpr char kTimingScope[] = "hardware";
constexpr unsigned kPhysicalTimingEvidence = kPerformanceOptimizedBuild ? 1u : 0u;
#else
constexpr char kTimingScope[] = "qemu_regression";
constexpr unsigned kPhysicalTimingEvidence = 0;
#endif

struct DiagnosticTimingSnapshot {
    std::uint32_t min_us = 0;
    std::uint32_t avg_us = 0;
    std::uint32_t p50_us = 0;
    std::uint32_t p95_us = 0;
    std::uint32_t p99_us = 0;
    std::uint32_t max_us = 0;
    std::uint32_t deadline_misses = 0;
};

voice::AdaptivePolyphonyConfig fixedBenchmarkPolicy() {
    voice::AdaptivePolyphonyConfig config{};
    config.block_budget_us = kBlockBudgetUs;
    config.high_pressure_percent = 80;
    config.recovery_pressure_percent = 50;
    config.high_pressure_blocks = 2;
    config.recovery_blocks = 4;
    config.min_limit = voice::VoicePool::kHardVoiceCap;
    config.initial_limit = voice::VoicePool::kHardVoiceCap;
    config.max_limit = voice::VoicePool::kHardVoiceCap;
    return config;
}

void initializePcm(std::array<std::int16_t, kPcmFrames>& pcm) {
    // Deterministic low-amplitude triangle-ish waveform. Even the 24-Voice
    // case stays in a useful bounded range while exercising interpolation,
    // Voice mixing, effects processing and final saturation checks.
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const std::int32_t phase = static_cast<std::int32_t>(i % 128u);
        const std::int32_t triangle = phase < 64 ? phase : 127 - phase;
        pcm[i] = static_cast<std::int16_t>((triangle - 32) * 32);
    }
}

voice::VoiceStartParams voiceParams(std::size_t index, bool effects_send = false) {
    voice::VoiceStartParams params{};
    params.sample_id = kBenchmarkSampleId;
    params.channel = static_cast<std::uint8_t>(index % 16u);
    params.key = static_cast<std::uint8_t>(48u + (index % 36u));
    params.velocity = 100;

    // Keep interpolation active with deterministic fractional increments while
    // staying close to 1x so every Voice repeatedly traverses the same loop.
    const std::uint64_t fractional =
        (voice::kPhaseOneQ32 / 97u) * static_cast<std::uint64_t>((index % 7u) + 1u);
    params.phase_increment_q32 = voice::kPhaseOneQ32 + fractional;
    params.loop_mode = voice::LoopMode::Continuous;
    params.gain_left_q15 = voice::kQ15Unity;
    params.gain_right_q15 = voice::kQ15Unity;
    params.envelope.attack_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = voice::kQ15Unity;
    params.envelope.release_frames = 0;
    if (effects_send) {
        params.reverb_send_units = kDiagnosticReverbSendUnits;
        params.chorus_send_units = kDiagnosticChorusSendUnits;
    }
    return params;
}

std::uint64_t updateChecksum(
    std::uint64_t checksum,
    const std::array<std::int16_t, realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock>& output,
    std::uint64_t* nonzero_samples) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int16_t sample_value : output) {
        const std::uint16_t bits = static_cast<std::uint16_t>(sample_value);
        checksum ^= bits;
        checksum *= kFnvPrime;
        if (sample_value != 0) {
            ++(*nonzero_samples);
        }
    }
    return checksum;
}

std::uint64_t updateWideChecksum(
    std::uint64_t checksum,
    const std::array<std::int32_t, realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock>& output) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int32_t sample_value : output) {
        checksum ^= static_cast<std::uint32_t>(sample_value);
        checksum *= kFnvPrime;
    }
    return checksum;
}

std::uint32_t elapsedUs(std::uint64_t start_us, std::uint64_t end_us) {
    if (end_us < start_us) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_us - start_us,
        std::numeric_limits<std::uint32_t>::max()));
}

DiagnosticTimingSnapshot summarizeDiagnosticTiming(
    const std::array<std::uint32_t, kMeasuredBlocks>& samples) {
    std::array<std::uint32_t, kMeasuredBlocks> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    std::uint64_t sum = 0;
    std::uint32_t deadline_misses = 0;
    for (const std::uint32_t sample : samples) {
        sum += sample;
        if (sample > kBlockBudgetUs) {
            ++deadline_misses;
        }
    }

    const auto percentile = [&sorted](std::size_t percent) {
        const std::size_t rank = (sorted.size() * percent + 99u) / 100u;
        return sorted[std::min<std::size_t>(
            rank == 0u ? 0u : rank - 1u,
            sorted.size() - 1u)];
    };

    DiagnosticTimingSnapshot result{};
    result.min_us = sorted.front();
    result.avg_us = static_cast<std::uint32_t>(sum / samples.size());
    result.p50_us = percentile(50u);
    result.p95_us = percentile(95u);
    result.p99_us = percentile(99u);
    result.max_us = sorted.back();
    result.deadline_misses = deadline_misses;
    return result;
}

void yieldToIdleTask() {
    // This qualification runs from app_main without the real I2S task blocking
    // cadence. Yield between measured cases so the IDLE0 watchdog can run. The
    // delay sits completely outside warmup/measured render intervals.
    vTaskDelay(1);
}

bool startDiagnosticVoices(voice::VoicePool& voices, std::size_t count, bool effects_send) {
    voices.allSoundOff();
    for (std::size_t index = 0; index < count; ++index) {
        if (!voices.start(voiceParams(index, effects_send))) {
            voices.allSoundOff();
            return false;
        }
    }
    return voices.activeVoiceCount() == count;
}

}  // namespace

bool runOptionalRenderTimingBenchmark() {
    static std::array<std::int16_t, kPcmFrames> pcm{};
    static sample::PublishedSampleDirectory directory;
    static sample::ResidentRecord record{};
    static voice::VoicePool voices(directory);
    static realtime::RealtimeEventQueue queue;
    static realtime::BlockEventScheduler scheduler(queue);
    static realtime::ScheduledAudioBlockRenderer renderer(voices, scheduler, nullptr);
    static const voice::AdaptivePolyphonyConfig policy = fixedBenchmarkPolicy();
    static voice::AdaptivePolyphonyController controller(voices, policy);
    static realtime::RenderDeadlineMonitor monitor(controller);
    static realtime::TimedAudioBlockRenderer timed_renderer(
        renderer,
        monitor,
        platform::espRealtimeNowUs,
        nullptr);
    static std::array<
        std::int16_t,
        realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> output{};

    initializePcm(pcm);
    record.bank_generation = directory.bankGeneration();
    record.sample_generation = 1;
    record.pcm = pcm.data();
    record.descriptor = sample::SampleDescriptor{};
    record.descriptor.id = kBenchmarkSampleId;
    record.descriptor.pcm_bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    record.descriptor.frames = static_cast<std::uint32_t>(pcm.size());
    record.descriptor.loop_start = 0;
    record.descriptor.loop_end = static_cast<std::uint32_t>(pcm.size());
    record.descriptor.sample_rate = kSampleRate;
    record.descriptor.root_key = 60;

    if (!directory.publish(kBenchmarkSampleId, &record)) {
        ESP_LOGE(kTag, "render_bench: publish failed");
        return false;
    }

    // Storage + Program Prepare run first in this one-shot image and can keep
    // app_main continuously runnable for several seconds. Reset that artificial
    // watchdog pressure before entering the timing matrix.
    yieldToIdleTask();

    ESP_LOGI(
        kTag,
        "render_bench: START frames_per_block=%u sample_rate=%u warmup_blocks=%u measured_blocks=%u budget_us=%u timing_scope=%s physical_timing_evidence=%u compiler_optimization=%s",
        static_cast<unsigned>(realtime::ScheduledAudioBlockRenderer::kFramesPerBlock),
        static_cast<unsigned>(kSampleRate),
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(kBlockBudgetUs),
        kTimingScope,
        kPhysicalTimingEvidence,
        kCompilerOptimization);

    std::uint64_t block_start_frame = 0;
    for (const std::size_t voice_count : kVoiceCounts) {
        voices.allSoundOff();
        if (directory.stats().live_voice_refs != 0u) {
            ESP_LOGE(
                kTag,
                "render_bench: stale refs before case voices=%u refs=%u",
                static_cast<unsigned>(voice_count),
                static_cast<unsigned>(directory.stats().live_voice_refs));
            return false;
        }

        for (std::size_t index = 0; index < voice_count; ++index) {
            if (!voices.start(voiceParams(index))) {
                ESP_LOGE(
                    kTag,
                    "render_bench: voice start failed voices=%u index=%u active=%u",
                    static_cast<unsigned>(voice_count),
                    static_cast<unsigned>(index),
                    static_cast<unsigned>(voices.activeVoiceCount()));
                voices.allSoundOff();
                return false;
            }
        }

        if (voices.activeVoiceCount() != voice_count ||
            directory.stats().live_voice_refs != voice_count) {
            ESP_LOGE(
                kTag,
                "render_bench: active/ref mismatch voices=%u active=%u refs=%u",
                static_cast<unsigned>(voice_count),
                static_cast<unsigned>(voices.activeVoiceCount()),
                static_cast<unsigned>(directory.stats().live_voice_refs));
            voices.allSoundOff();
            return false;
        }

        for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
            renderer.render(block_start_frame, output.data());
            block_start_frame += realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
        }

        const std::uint64_t misses_before = monitor.stats().deadline_misses;
        const std::uint64_t clipped_before = renderer.stats().clipped_samples;
        std::uint64_t checksum = 1469598103934665603ull;
        std::uint64_t nonzero_samples = 0;

        for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
            (void)timed_renderer.render(block_start_frame, output.data());
            block_start_frame += realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
            checksum = updateChecksum(checksum, output, &nonzero_samples);
        }

        const realtime::RenderTimingSnapshot timing = monitor.snapshot();
        const std::uint64_t case_misses = timing.deadline_misses - misses_before;
        const std::uint64_t case_clipped = renderer.stats().clipped_samples - clipped_before;

        if (timing.sample_count != kMeasuredBlocks || nonzero_samples == 0u) {
            ESP_LOGE(
                kTag,
                "render_bench: invalid telemetry voices=%u samples=%u nonzero=%llu",
                static_cast<unsigned>(voice_count),
                static_cast<unsigned>(timing.sample_count),
                static_cast<unsigned long long>(nonzero_samples));
            voices.allSoundOff();
            return false;
        }

        ESP_LOGI(
            kTag,
            "render_bench: case voices=%u blocks=%u min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%llu clipped_samples=%llu checksum=%016llx",
            static_cast<unsigned>(voice_count),
            static_cast<unsigned>(kMeasuredBlocks),
            static_cast<unsigned>(timing.min_us),
            static_cast<unsigned>(timing.avg_us),
            static_cast<unsigned>(timing.p50_us),
            static_cast<unsigned>(timing.p95_us),
            static_cast<unsigned>(timing.p99_us),
            static_cast<unsigned>(timing.max_us),
            static_cast<unsigned long long>(case_misses),
            static_cast<unsigned long long>(case_clipped),
            static_cast<unsigned long long>(checksum));

        yieldToIdleTask();
    }

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
    // One physical run should also tell us where any remaining deadline cost
    // lives. These diagnostics do not change the primary dry 1..24 Voice gate.
    static std::array<
        std::int32_t,
        realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> diagnostic_mix{};
    static std::array<
        std::int32_t,
        realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> diagnostic_reverb{};
    static std::array<
        std::int32_t,
        realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock> diagnostic_chorus{};
    static std::array<std::uint32_t, kMeasuredBlocks> diagnostic_samples{};

    yieldToIdleTask();
    if (!startDiagnosticVoices(voices, kDiagnosticVoices, false)) {
        ESP_LOGE(kTag, "render_diag: voice_only start failed");
        return false;
    }
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        diagnostic_mix.fill(0);
        diagnostic_reverb.fill(0);
        diagnostic_chorus.fill(0);
        voices.renderAddWithEffects(
            diagnostic_mix.data(),
            diagnostic_reverb.data(),
            diagnostic_chorus.data(),
            realtime::ScheduledAudioBlockRenderer::kFramesPerBlock);
    }
    std::uint64_t voice_only_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        diagnostic_mix.fill(0);
        diagnostic_reverb.fill(0);
        diagnostic_chorus.fill(0);
        const std::uint64_t start_us = platform::espRealtimeNowUs();
        voices.renderAddWithEffects(
            diagnostic_mix.data(),
            diagnostic_reverb.data(),
            diagnostic_chorus.data(),
            realtime::ScheduledAudioBlockRenderer::kFramesPerBlock);
        const std::uint64_t end_us = platform::espRealtimeNowUs();
        diagnostic_samples[block] = elapsedUs(start_us, end_us);
        voice_only_checksum = updateWideChecksum(voice_only_checksum, diagnostic_mix);
    }
    const DiagnosticTimingSnapshot voice_only = summarizeDiagnosticTiming(diagnostic_samples);
    ESP_LOGI(
        kTag,
        "render_diag: stage=voice_only voices=%u blocks=%u min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u checksum=%016llx",
        static_cast<unsigned>(kDiagnosticVoices),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(voice_only.min_us),
        static_cast<unsigned>(voice_only.avg_us),
        static_cast<unsigned>(voice_only.p50_us),
        static_cast<unsigned>(voice_only.p95_us),
        static_cast<unsigned>(voice_only.p99_us),
        static_cast<unsigned>(voice_only.max_us),
        static_cast<unsigned>(voice_only.deadline_misses),
        static_cast<unsigned long long>(voice_only_checksum));
    voices.allSoundOff();

    yieldToIdleTask();
    if (!startDiagnosticVoices(voices, kDiagnosticVoices, true)) {
        ESP_LOGE(kTag, "render_diag: full_fx start failed");
        return false;
    }
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        renderer.render(block_start_frame, output.data());
        block_start_frame += realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
    }
    const std::uint64_t fx_clipped_before = renderer.stats().clipped_samples;
    std::uint64_t fx_checksum = 1469598103934665603ull;
    std::uint64_t fx_nonzero_samples = 0;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        const std::uint64_t start_us = platform::espRealtimeNowUs();
        renderer.render(block_start_frame, output.data());
        const std::uint64_t end_us = platform::espRealtimeNowUs();
        block_start_frame += realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
        diagnostic_samples[block] = elapsedUs(start_us, end_us);
        fx_checksum = updateChecksum(fx_checksum, output, &fx_nonzero_samples);
    }
    const DiagnosticTimingSnapshot full_fx = summarizeDiagnosticTiming(diagnostic_samples);
    const std::uint64_t fx_clipped = renderer.stats().clipped_samples - fx_clipped_before;
    ESP_LOGI(
        kTag,
        "render_diag: stage=full_fx voices=%u blocks=%u reverb_send=%d chorus_send=%d min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u clipped_samples=%llu checksum=%016llx",
        static_cast<unsigned>(kDiagnosticVoices),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<int>(kDiagnosticReverbSendUnits),
        static_cast<int>(kDiagnosticChorusSendUnits),
        static_cast<unsigned>(full_fx.min_us),
        static_cast<unsigned>(full_fx.avg_us),
        static_cast<unsigned>(full_fx.p50_us),
        static_cast<unsigned>(full_fx.p95_us),
        static_cast<unsigned>(full_fx.p99_us),
        static_cast<unsigned>(full_fx.max_us),
        static_cast<unsigned>(full_fx.deadline_misses),
        static_cast<unsigned long long>(fx_clipped),
        static_cast<unsigned long long>(fx_checksum));
    if (fx_nonzero_samples == 0u) {
        ESP_LOGE(kTag, "render_diag: full_fx produced only silence");
        voices.allSoundOff();
        return false;
    }
    voices.allSoundOff();
    yieldToIdleTask();
#endif

    voices.allSoundOff();
    const auto publication = directory.stats();
    if (publication.live_voice_refs != 0u) {
        ESP_LOGE(
            kTag,
            "render_bench: leaked voice refs=%u",
            static_cast<unsigned>(publication.live_voice_refs));
        return false;
    }

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
    ESP_LOGI(kTag, "render_diag: PASS final_refs=0");
#endif

    ESP_LOGI(
        kTag,
        "render_bench: PASS cases=%u final_refs=0 clock_faults=%llu duration_clamps=%llu timing_scope=%s physical_timing_evidence=%u compiler_optimization=%s",
        static_cast<unsigned>(kVoiceCounts.size()),
        static_cast<unsigned long long>(timed_renderer.stats().clock_faults),
        static_cast<unsigned long long>(timed_renderer.stats().duration_clamps),
        kTimingScope,
        kPhysicalTimingEvidence,
        kCompilerOptimization);
    return true;
}

}  // namespace gm_synth::bringup

#else

namespace gm_synth::bringup {

bool runOptionalRenderTimingBenchmark() {
    return true;
}

}  // namespace gm_synth::bringup

#endif
