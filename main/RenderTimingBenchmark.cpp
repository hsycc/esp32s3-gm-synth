#include "RenderTimingBenchmark.h"

#include "sdkconfig.h"

#ifdef CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE

#include "esp_log.h"

#include "platform/EspRealtimeClock.h"
#include "realtime/BlockEventScheduler.h"
#include "realtime/RealtimeEventQueue.h"
#include "realtime/RenderDeadlineMonitor.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "realtime/TimedAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/AdaptivePolyphony.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cstddef>
#include <cstdint>

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

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
constexpr char kTimingScope[] = "hardware";
constexpr unsigned kPhysicalTimingEvidence = 1;
#else
constexpr char kTimingScope[] = "qemu_regression";
constexpr unsigned kPhysicalTimingEvidence = 0;
#endif

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

voice::VoiceStartParams voiceParams(std::size_t index) {
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

    ESP_LOGI(
        kTag,
        "render_bench: START frames_per_block=%u sample_rate=%u warmup_blocks=%u measured_blocks=%u budget_us=%u timing_scope=%s physical_timing_evidence=%u",
        static_cast<unsigned>(realtime::ScheduledAudioBlockRenderer::kFramesPerBlock),
        static_cast<unsigned>(kSampleRate),
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(kBlockBudgetUs),
        kTimingScope,
        kPhysicalTimingEvidence);

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
    }

    voices.allSoundOff();
    const auto publication = directory.stats();
    if (publication.live_voice_refs != 0u) {
        ESP_LOGE(
            kTag,
            "render_bench: leaked voice refs=%u",
            static_cast<unsigned>(publication.live_voice_refs));
        return false;
    }

    ESP_LOGI(
        kTag,
        "render_bench: PASS cases=%u final_refs=0 clock_faults=%llu duration_clamps=%llu timing_scope=%s physical_timing_evidence=%u",
        static_cast<unsigned>(kVoiceCounts.size()),
        static_cast<unsigned long long>(timed_renderer.stats().clock_faults),
        static_cast<unsigned long long>(timed_renderer.stats().duration_clamps),
        kTimingScope,
        kPhysicalTimingEvidence);
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
