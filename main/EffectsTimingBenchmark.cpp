#include "EffectsTimingBenchmark.h"
#include "I2sDmaQualification.h"

#include "sdkconfig.h"

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
#include "PhysicalHeavyDspStress.h"
#include "PhysicalMusicStress.h"
#endif

#ifdef CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE

#include "esp_log.h"
#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
#include "esp_heap_caps.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform/EspRealtimeClock.h"
#include "realtime/BlockEventScheduler.h"
#include "realtime/RealtimeEventQueue.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/GlobalEffectsProcessor.h"
#include "voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
#include <new>
#endif

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr sample::SampleId kBenchmarkSampleId = 0;
constexpr std::size_t kPcmFrames = 1024;
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kDiagnosticVoices = 16;
constexpr std::size_t kWarmupBlocks = 16;
constexpr std::size_t kMeasuredBlocks = 128;
constexpr std::int16_t kReverbSendUnits = 500;
constexpr std::int16_t kChorusSendUnits = 500;
constexpr std::size_t kSamplesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock;
constexpr std::size_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;

#ifdef CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD
constexpr char kTimingScope[] = "hardware";
#if defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
constexpr unsigned kPhysicalTimingEvidence = 1u;
#else
constexpr unsigned kPhysicalTimingEvidence = 0u;
#endif
#else
constexpr char kTimingScope[] = "qemu_regression";
constexpr unsigned kPhysicalTimingEvidence = 0u;
#endif

struct TimingSnapshot {
    std::uint32_t min_us = 0;
    std::uint32_t avg_us = 0;
    std::uint32_t p50_us = 0;
    std::uint32_t p95_us = 0;
    std::uint32_t p99_us = 0;
    std::uint32_t max_us = 0;
    std::uint32_t deadline_misses = 0;
};

void initializePcm(std::array<std::int16_t, kPcmFrames>& pcm) {
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const std::int32_t phase = static_cast<std::int32_t>(i % 128u);
        const std::int32_t triangle = phase < 64 ? phase : 127 - phase;
        pcm[i] = static_cast<std::int16_t>((triangle - 32) * 32);
    }
}

voice::VoiceStartParams voiceParams(std::size_t index, bool effects_send) {
    voice::VoiceStartParams params{};
    params.sample_id = kBenchmarkSampleId;
    params.channel = static_cast<std::uint8_t>(index % 16u);
    params.key = static_cast<std::uint8_t>(48u + (index % 36u));
    params.velocity = 100;
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
        params.reverb_send_units = kReverbSendUnits;
        params.chorus_send_units = kChorusSendUnits;
    }
    return params;
}

bool startVoices(voice::VoicePool& voices, bool effects_send) {
    voices.allSoundOff();
    for (std::size_t index = 0; index < kDiagnosticVoices; ++index) {
        if (!voices.start(voiceParams(index, effects_send))) {
            voices.allSoundOff();
            return false;
        }
    }
    return voices.activeVoiceCount() == kDiagnosticVoices;
}

std::uint32_t elapsedUs(std::uint64_t start_us, std::uint64_t end_us) {
    if (end_us < start_us) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_us - start_us,
        std::numeric_limits<std::uint32_t>::max()));
}

TimingSnapshot summarize(const std::array<std::uint32_t, kMeasuredBlocks>& samples) {
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

    TimingSnapshot result{};
    result.min_us = sorted.front();
    result.avg_us = static_cast<std::uint32_t>(sum / samples.size());
    result.p50_us = percentile(50u);
    result.p95_us = percentile(95u);
    result.p99_us = percentile(99u);
    result.max_us = sorted.back();
    result.deadline_misses = deadline_misses;
    return result;
}

std::uint64_t updateWideChecksum(
    std::uint64_t checksum,
    const std::array<std::int32_t, kSamplesPerBlock>& values) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int32_t value : values) {
        checksum ^= static_cast<std::uint32_t>(value);
        checksum *= kFnvPrime;
    }
    return checksum;
}

std::uint64_t updateI16Checksum(
    std::uint64_t checksum,
    const std::array<std::int16_t, kSamplesPerBlock>& values) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int16_t value : values) {
        checksum ^= static_cast<std::uint16_t>(value);
        checksum *= kFnvPrime;
    }
    return checksum;
}

void fillEffectsInput(
    std::array<std::int32_t, kSamplesPerBlock>& reverb,
    std::array<std::int32_t, kSamplesPerBlock>& chorus) {
    for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
        const std::int32_t phase = static_cast<std::int32_t>((frame * 37u + 11u) % 97u);
        const std::int32_t base = (phase - 48) * 173;
        reverb[frame * 2u] = base;
        reverb[frame * 2u + 1u] = base / 3;
        chorus[frame * 2u] = -base / 2;
        chorus[frame * 2u + 1u] = base;
    }
}

void yieldToIdleTask() {
    vTaskDelay(1);
}

void logStage(const char* stage, const TimingSnapshot& timing, std::uint64_t checksum) {
    ESP_LOGI(
        kTag,
        "effects_bench: stage=%s voices=%u blocks=%u min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u checksum=%016llx timing_scope=%s physical_timing_evidence=%u",
        stage,
        static_cast<unsigned>(kDiagnosticVoices),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(timing.min_us),
        static_cast<unsigned>(timing.avg_us),
        static_cast<unsigned>(timing.p50_us),
        static_cast<unsigned>(timing.p95_us),
        static_cast<unsigned>(timing.p99_us),
        static_cast<unsigned>(timing.max_us),
        static_cast<unsigned>(timing.deadline_misses),
        static_cast<unsigned long long>(checksum),
        kTimingScope,
        kPhysicalTimingEvidence);
}

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
PhysicalMusicStress* allocatePhysicalMusicStress(
    sample::PublishedSampleDirectory& directory) {
    // PhysicalMusicStress embeds the production-shaped Program Event applier,
    // whose 32-Voice resolve batch is intentionally large. Keeping this object
    // as a function-static put that entire batch in .dram0.bss and overflowed
    // the ESP32-S3 static DRAM link region. Allocate it once from INTERNAL heap
    // instead: the realtime callback still runs from internal SRAM, while link-
    // time BSS stays bounded. The renderer stores this pointer for process life,
    // so this one-shot qualification allocation is deliberately not freed.
    void* storage = heap_caps_malloc(
        sizeof(PhysicalMusicStress),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (storage == nullptr) {
        ESP_LOGE(
            kTag,
            "physical_stress: internal allocation failed bytes=%u free_internal=%u",
            static_cast<unsigned>(sizeof(PhysicalMusicStress)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        return nullptr;
    }

    auto* stress = new (storage) PhysicalMusicStress(directory);
    ESP_LOGI(
        kTag,
        "physical_stress: INTERNAL_STATE bytes=%u free_internal_after=%u",
        static_cast<unsigned>(sizeof(PhysicalMusicStress)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return stress;
}
#endif

}  // namespace

bool runOptionalEffectsTimingBenchmark() {
    static std::array<std::int16_t, kPcmFrames> pcm{};
    static sample::PublishedSampleDirectory directory;
    static sample::ResidentRecord record{};
    static voice::VoicePool voices(directory);
    static realtime::RealtimeEventQueue queue;
    static realtime::BlockEventScheduler scheduler(queue);
#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    static PhysicalMusicStress* physical_stress = allocatePhysicalMusicStress(directory);
    if (physical_stress == nullptr) {
        return false;
    }
    static realtime::ScheduledAudioBlockRenderer renderer(
        voices,
        scheduler,
        &PhysicalMusicStress::callback,
        physical_stress);
#else
    static realtime::ScheduledAudioBlockRenderer renderer(voices, scheduler, nullptr);
#endif
    static voice::GlobalEffectsProcessor effects;
    static std::array<std::int32_t, kSamplesPerBlock> mix{};
    static std::array<std::int32_t, kSamplesPerBlock> reverb{};
    static std::array<std::int32_t, kSamplesPerBlock> chorus{};
    static std::array<std::int16_t, kSamplesPerBlock> output{};
    static std::array<std::uint32_t, kMeasuredBlocks> timings{};

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
        ESP_LOGE(kTag, "effects_bench: publish failed");
        return false;
    }

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    if (!physical_stress->initialize()) {
        ESP_LOGE(kTag, "effects_bench: physical music stress initialization failed");
        return false;
    }
#endif

    ESP_LOGI(
        kTag,
        "effects_bench: START voices=%u frames_per_block=%u warmup_blocks=%u measured_blocks=%u reverb_send=%d chorus_send=%d timing_scope=%s physical_timing_evidence=%u",
        static_cast<unsigned>(kDiagnosticVoices),
        static_cast<unsigned>(kFramesPerBlock),
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<int>(kReverbSendUnits),
        static_cast<int>(kChorusSendUnits),
        kTimingScope,
        kPhysicalTimingEvidence);

    // Stage 1: Voice rendering plus per-voice send-bus scaling, but no global
    // Chorus/Reverb processing. This isolates scratch/send distribution cost.
    if (!startVoices(voices, true)) {
        ESP_LOGE(kTag, "effects_bench: send_bus voice start failed");
        return false;
    }
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        mix.fill(0);
        reverb.fill(0);
        chorus.fill(0);
        voices.renderAddWithEffects(mix.data(), reverb.data(), chorus.data(), kFramesPerBlock);
    }
    std::uint64_t send_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        mix.fill(0);
        reverb.fill(0);
        chorus.fill(0);
        const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
        voices.renderAddWithEffects(mix.data(), reverb.data(), chorus.data(), kFramesPerBlock);
        const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
        timings[block] = elapsedUs(start_us, end_us);
        send_checksum = updateWideChecksum(send_checksum, reverb);
        send_checksum = updateWideChecksum(send_checksum, chorus);
    }
    logStage("send_bus", summarize(timings), send_checksum);
    voices.allSoundOff();
    yieldToIdleTask();

    // Stage 2: global Chorus/Reverb alone on a deterministic non-zero bus. This
    // makes QEMU useful for comparing effect-core implementation changes.
    fillEffectsInput(reverb, chorus);
    effects.reset();
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        mix.fill(0);
        effects.processAdd(chorus.data(), reverb.data(), mix.data(), kFramesPerBlock);
    }
    std::uint64_t global_fx_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        mix.fill(0);
        const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
        effects.processAdd(chorus.data(), reverb.data(), mix.data(), kFramesPerBlock);
        const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
        timings[block] = elapsedUs(start_us, end_us);
        global_fx_checksum = updateWideChecksum(global_fx_checksum, mix);
    }
    logStage("global_fx", summarize(timings), global_fx_checksum);
    yieldToIdleTask();

    // Stage 3: complete scheduled renderer with 16 Voices and 50% sends. This
    // is the end-to-end FX regression row; QEMU values remain trend-only.
    if (!startVoices(voices, true)) {
        ESP_LOGE(kTag, "effects_bench: full_fx voice start failed");
        return false;
    }
    std::uint64_t block_start_frame = 0;
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        renderer.render(block_start_frame, output.data());
        block_start_frame += kFramesPerBlock;
    }
    const std::uint64_t clipped_before = renderer.stats().clipped_samples;
    std::uint64_t full_fx_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
        renderer.render(block_start_frame, output.data());
        const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
        block_start_frame += kFramesPerBlock;
        timings[block] = elapsedUs(start_us, end_us);
        full_fx_checksum = updateI16Checksum(full_fx_checksum, output);
    }
    const TimingSnapshot full_fx = summarize(timings);
    const std::uint64_t clipped = renderer.stats().clipped_samples - clipped_before;
    ESP_LOGI(
        kTag,
        "effects_bench: stage=full_fx voices=%u blocks=%u min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u clipped_samples=%llu checksum=%016llx timing_scope=%s physical_timing_evidence=%u",
        static_cast<unsigned>(kDiagnosticVoices),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(full_fx.min_us),
        static_cast<unsigned>(full_fx.avg_us),
        static_cast<unsigned>(full_fx.p50_us),
        static_cast<unsigned>(full_fx.p95_us),
        static_cast<unsigned>(full_fx.p99_us),
        static_cast<unsigned>(full_fx.max_us),
        static_cast<unsigned>(full_fx.deadline_misses),
        static_cast<unsigned long long>(clipped),
        static_cast<unsigned long long>(full_fx_checksum),
        kTimingScope,
        kPhysicalTimingEvidence);

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    // Establish both the ordinary full-FX ceiling and a generic heavy-DSP
    // ceiling before the long physical stream. The product candidate is the
    // conservative minimum of those independently measured consecutive limits.
    voices.allSoundOff();
    if (!physical_stress->runPolyphonySweep(voices, renderer, &block_start_frame)) {
        ESP_LOGE(kTag, "effects_bench: physical polyphony qualification FAILED");
        voices.allSoundOff();
        return false;
    }

    const PhysicalHeavyDspStressResult heavy = runPhysicalHeavyDspStress(
        directory,
        voices,
        renderer,
        &block_start_frame);
    const std::size_t product_candidate = std::min(
        physical_stress->maxStablePolyphony(),
        heavy.max_stable_voices);
    ESP_LOGI(
        kTag,
        "physical_product: RESULT full_fx_max=%u heavy_dsp_max=%u candidate=%u heavy_minimum_required=%u %s",
        static_cast<unsigned>(physical_stress->maxStablePolyphony()),
        static_cast<unsigned>(heavy.max_stable_voices),
        static_cast<unsigned>(product_candidate),
        static_cast<unsigned>(heavy.minimum_required_voices),
        heavy.pass() ? "PASS" : "FAIL");
    if (!heavy.pass()) {
        ESP_LOGE(kTag, "effects_bench: physical heavy-DSP qualification FAILED");
        voices.allSoundOff();
        return false;
    }

    if (!physical_stress->enqueueTimeline(
            queue,
            voices,
            renderer,
            block_start_frame,
            CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS)) {
        ESP_LOGE(kTag, "effects_bench: physical music stress timeline setup FAILED");
        voices.allSoundOff();
        return false;
    }
#endif

    if (!runOptionalI2sDmaQualification(renderer, block_start_frame)) {
        ESP_LOGE(kTag, "effects_bench: physical I2S/DMA qualification FAILED");
        voices.allSoundOff();
        return false;
    }

#ifdef CONFIG_GM_SYNTH_I2S_QUAL_ENABLE
    if (!physical_stress->validateAndLog(queue, voices, renderer)) {
        ESP_LOGE(kTag, "effects_bench: physical music stress qualification FAILED");
        voices.allSoundOff();
        return false;
    }
#endif

    voices.allSoundOff();

    if (directory.stats().live_voice_refs != 0u) {
        ESP_LOGE(
            kTag,
            "effects_bench: leaked voice refs=%u",
            static_cast<unsigned>(directory.stats().live_voice_refs));
        return false;
    }

    ESP_LOGI(kTag, "effects_bench: PASS final_refs=0");
    yieldToIdleTask();
    return true;
}

}  // namespace gm_synth::bringup

#else

namespace gm_synth::bringup {

bool runOptionalEffectsTimingBenchmark() {
    return true;
}

}  // namespace gm_synth::bringup

#endif