#include "DspStageTimingBenchmark.h"

#include "sdkconfig.h"

#ifdef CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gm/MidiChannelState.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "platform/EspRealtimeClock.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/GlobalEffectsProcessor.h"
#include "voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr sample::SampleId kSampleId = 0;
constexpr std::size_t kPcmFrames = 1024;
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kVoices = 16;
constexpr std::size_t kWarmupBlocks = 16;
constexpr std::size_t kMeasuredBlocks = 128;
constexpr std::size_t kFramesPerBlock = 64;
constexpr std::size_t kSamplesPerBlock = kFramesPerBlock * 2u;
constexpr std::int16_t kEffectsSendUnits = 500;

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

enum class VoiceStage : std::uint8_t {
    Dry = 0,
    SendBus,
    StaticFilter,
    VibratoLfo,
    ModLfoPitch,
    ModLfoFull,
    ModEnvelope,
    HeavyDsp,
};

enum class FxStage : std::uint8_t {
    Chorus = 0,
    Reverb,
    ChorusReverb,
};

struct TimingSnapshot {
    std::uint32_t min_us = 0;
    std::uint32_t avg_us = 0;
    std::uint32_t p50_us = 0;
    std::uint32_t p95_us = 0;
    std::uint32_t p99_us = 0;
    std::uint32_t max_us = 0;
    std::uint32_t deadline_misses = 0;
};

struct VoiceBenchFixture {
    sample::PublishedSampleDirectory directory;
    voice::VoicePool voices;

    VoiceBenchFixture()
        : directory(),
          voices(directory) {}
};

std::uint32_t elapsedUs(std::uint64_t start_us, std::uint64_t end_us) {
    if (end_us < start_us) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_us - start_us,
        std::numeric_limits<std::uint32_t>::max()));
}

TimingSnapshot summarize(const std::array<std::uint32_t, kMeasuredBlocks>& values) {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());

    std::uint64_t sum = 0;
    std::uint32_t misses = 0;
    for (const std::uint32_t value : values) {
        sum += value;
        if (value > kBlockBudgetUs) {
            ++misses;
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
    result.avg_us = static_cast<std::uint32_t>(sum / values.size());
    result.p50_us = percentile(50u);
    result.p95_us = percentile(95u);
    result.p99_us = percentile(99u);
    result.max_us = sorted.back();
    result.deadline_misses = misses;
    return result;
}

std::uint32_t relativeMilli(std::uint32_t value, std::uint32_t baseline) {
    if (baseline == 0u) {
        return 0u;
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(value) * 1000u + baseline / 2u;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        scaled / baseline,
        std::numeric_limits<std::uint32_t>::max()));
}

std::int32_t deltaUs(std::uint32_t value, std::uint32_t baseline) {
    const std::int64_t delta =
        static_cast<std::int64_t>(value) - static_cast<std::int64_t>(baseline);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        delta,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

std::uint64_t updateChecksum(
    std::uint64_t checksum,
    const std::int32_t* values,
    std::size_t count) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    if (values == nullptr) {
        return checksum;
    }
    for (std::size_t i = 0; i < count; ++i) {
        checksum ^= static_cast<std::uint32_t>(values[i]);
        checksum *= kFnvPrime;
    }
    return checksum;
}

void initializePcm(std::int16_t* pcm) {
    for (std::size_t i = 0; i < kPcmFrames; ++i) {
        const std::int32_t phase = static_cast<std::int32_t>(i % 128u);
        const std::int32_t triangle = phase < 64 ? phase : 127 - phase;
        pcm[i] = static_cast<std::int16_t>((triangle - 32) * 32);
    }
}

void enableStaticFilter(gm::PlaybackZone* zone) {
    zone->note_on_filter.enabled = true;
    zone->note_on_filter.initial_cutoff_cents = 8000;
    zone->note_on_filter.velocity_to_cutoff_cents = 0;
    zone->note_on_filter.resonance_cb = 100;
}

void enableVibrato(gm::PlaybackZone* zone) {
    zone->vibrato_lfo.enabled = true;
    zone->vibrato_lfo.delay_frames = 0;
    zone->vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 19u;
    zone->vibrato_lfo.static_pitch_depth_cents = 50;
    zone->vibrato_lfo.channel_pressure_to_pitch_cents = 50;
    zone->vibrato_lfo.modulation_wheel_to_pitch_cents = 50;
}

void enableModLfoPitch(gm::PlaybackZone* zone) {
    zone->mod_lfo.enabled = true;
    zone->mod_lfo.delay_frames = 0;
    zone->mod_lfo.phase_step_q32 = std::uint32_t{1} << 20u;
    zone->mod_lfo.pitch_depth_cents = 100;
}

void enableModLfoFull(gm::PlaybackZone* zone) {
    enableModLfoPitch(zone);
    zone->mod_lfo.filter_depth_cents = 1200;
    zone->mod_lfo.volume_depth_cb = 120;
}

void enableModEnvelope(gm::PlaybackZone* zone) {
    zone->mod_envelope.enabled = true;
    zone->mod_envelope.envelope.attack_frames = 128;
    zone->mod_envelope.envelope.hold_frames = 64;
    zone->mod_envelope.envelope.decay_frames = 256;
    zone->mod_envelope.envelope.sustain_q15 = voice::kQ15Unity / 2;
    zone->mod_envelope.envelope.release_frames = 4410;
    zone->mod_envelope.pitch_depth_cents = 600;
    zone->mod_envelope.filter_depth_cents = 600;
}

void enableEffectsSend(gm::PlaybackZone* zone) {
    zone->effects_send.enabled = true;
    zone->effects_send.reverb_sf2_units = kEffectsSendUnits;
    zone->effects_send.chorus_sf2_units = kEffectsSendUnits;
    zone->effects_send.reverb_cc_amount = 0;
    zone->effects_send.chorus_cc_amount = 0;
}

gm::PlaybackZone makeZone(VoiceStage stage) {
    gm::PlaybackZone zone{};
    zone.sample_id = kSampleId;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.root_key = 60;
    zone.key_tracking = true;
    zone.scale_tuning_cents_per_key = 100;
    zone.root_phase_increment_q32 = voice::kPhaseOneQ32;
    zone.loop_mode = voice::LoopMode::Continuous;

    // Keep all stage outputs comparable and far from clipping. The same
    // NoteOn gain baseline is used by the physical heavy-DSP fixture.
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 240;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 0;
    zone.note_on_gain.pan_amount = 0;
    zone.note_on_gain.expression_to_attenuation_cb = 0;
    zone.note_on_gain.pan_sf2_units = 0;

    zone.envelope.attack_frames = 0;
    zone.envelope.decay_frames = 0;
    zone.envelope.sustain_q15 = voice::kQ15Unity;
    zone.envelope.release_frames = 4410;

    switch (stage) {
        case VoiceStage::Dry:
            break;
        case VoiceStage::SendBus:
            enableEffectsSend(&zone);
            break;
        case VoiceStage::StaticFilter:
            enableStaticFilter(&zone);
            break;
        case VoiceStage::VibratoLfo:
            enableVibrato(&zone);
            break;
        case VoiceStage::ModLfoPitch:
            enableModLfoPitch(&zone);
            break;
        case VoiceStage::ModLfoFull:
            enableStaticFilter(&zone);
            enableModLfoFull(&zone);
            break;
        case VoiceStage::ModEnvelope:
            enableStaticFilter(&zone);
            enableModEnvelope(&zone);
            break;
        case VoiceStage::HeavyDsp:
            enableStaticFilter(&zone);
            enableVibrato(&zone);
            enableModLfoFull(&zone);
            enableModEnvelope(&zone);
            break;
    }
    return zone;
}

const char* voiceStageName(VoiceStage stage) {
    switch (stage) {
        case VoiceStage::Dry:
            return "voice_dry";
        case VoiceStage::SendBus:
            return "voice_send_bus";
        case VoiceStage::StaticFilter:
            return "voice_static_filter";
        case VoiceStage::VibratoLfo:
            return "voice_vibrato_lfo";
        case VoiceStage::ModLfoPitch:
            return "voice_mod_lfo_pitch";
        case VoiceStage::ModLfoFull:
            return "voice_mod_lfo_pitch_filter_volume";
        case VoiceStage::ModEnvelope:
            return "voice_mod_env_pitch_filter";
        case VoiceStage::HeavyDsp:
            return "voice_heavy_dsp";
    }
    return "voice_unknown";
}

const char* fxStageName(FxStage stage) {
    switch (stage) {
        case FxStage::Chorus:
            return "global_chorus";
        case FxStage::Reverb:
            return "global_reverb";
        case FxStage::ChorusReverb:
            return "global_chorus_reverb";
    }
    return "global_unknown";
}

gm::PlaybackProgramSnapshot makeProgram(
    const gm::PlaybackZone* zone,
    std::uint32_t bank_generation) {
    gm::PlaybackProgramSnapshot program{};
    program.channel = 0;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = bank_generation;
    program.zones = zone;
    program.zone_count = 1;
    return program;
}

bool startStageVoices(
    VoiceStage stage,
    VoiceBenchFixture& fixture,
    gm::ResolvedVoiceBatch& batch) {
    const gm::PlaybackZone zone = makeZone(stage);
    const gm::PlaybackProgramSnapshot program =
        makeProgram(&zone, fixture.directory.bankGeneration());
    if (!zone.valid() || !program.valid()) {
        ESP_LOGE(
            kTag,
            "dsp_stage_bench: invalid fixture stage=%s zone_valid=%u program_valid=%u",
            voiceStageName(stage),
            zone.valid() ? 1u : 0u,
            program.valid() ? 1u : 0u);
        return false;
    }

    gm::MidiChannelState channel_state{};
    channel_state.resetAll();
    channel_state.volume = 127;
    channel_state.expression = 127;
    channel_state.pan = 64;

    gm::ZoneResolver resolver;
    fixture.voices.allSoundOff();
    for (std::size_t index = 0; index < kVoices; ++index) {
        batch.clear();
        const std::uint8_t key =
            static_cast<std::uint8_t>(48u + (index % 36u));
        const gm::ZoneResolveStatus status = resolver.resolveNoteOn(
            program,
            channel_state,
            0,
            key,
            100,
            static_cast<std::uint32_t>(index + 1u),
            &batch);
        if (status != gm::ZoneResolveStatus::Ok ||
            batch.count != 1u ||
            !fixture.voices.start(batch.voices[0])) {
            ESP_LOGE(
                kTag,
                "dsp_stage_bench: voice setup failed stage=%s index=%u status=%u resolved=%u active=%u",
                voiceStageName(stage),
                static_cast<unsigned>(index),
                static_cast<unsigned>(status),
                static_cast<unsigned>(batch.count),
                static_cast<unsigned>(fixture.voices.activeVoiceCount()));
            fixture.voices.allSoundOff();
            return false;
        }
    }

    if (fixture.voices.activeVoiceCount() != kVoices) {
        ESP_LOGE(
            kTag,
            "dsp_stage_bench: active voice mismatch stage=%s expected=%u actual=%u",
            voiceStageName(stage),
            static_cast<unsigned>(kVoices),
            static_cast<unsigned>(fixture.voices.activeVoiceCount()));
        fixture.voices.allSoundOff();
        return false;
    }
    return true;
}

void clearBuses(
    std::int32_t* mix,
    std::int32_t* reverb,
    std::int32_t* chorus) {
    std::fill_n(mix, kSamplesPerBlock, 0);
    std::fill_n(reverb, kSamplesPerBlock, 0);
    std::fill_n(chorus, kSamplesPerBlock, 0);
}

TimingSnapshot measureVoiceStage(
    VoiceStage stage,
    voice::VoicePool& voices,
    std::int32_t* mix,
    std::int32_t* reverb,
    std::int32_t* chorus,
    std::uint64_t* checksum) {
    const bool with_send_bus = stage == VoiceStage::SendBus;

    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        clearBuses(mix, reverb, chorus);
        if (with_send_bus) {
            voices.renderAddWithEffects(
                mix,
                reverb,
                chorus,
                kFramesPerBlock);
        } else {
            voices.renderAdd(mix, kFramesPerBlock);
        }
    }

    std::array<std::uint32_t, kMeasuredBlocks> timings{};
    std::uint64_t stage_checksum = 1469598103934665603ull;
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        clearBuses(mix, reverb, chorus);
        const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
        if (with_send_bus) {
            voices.renderAddWithEffects(
                mix,
                reverb,
                chorus,
                kFramesPerBlock);
        } else {
            voices.renderAdd(mix, kFramesPerBlock);
        }
        const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
        timings[block] = elapsedUs(start_us, end_us);
        stage_checksum = updateChecksum(
            stage_checksum,
            mix,
            kSamplesPerBlock);
        if (with_send_bus) {
            stage_checksum = updateChecksum(
                stage_checksum,
                reverb,
                kSamplesPerBlock);
            stage_checksum = updateChecksum(
                stage_checksum,
                chorus,
                kSamplesPerBlock);
        }
    }
    if (checksum != nullptr) {
        *checksum = stage_checksum;
    }
    return summarize(timings);
}

void logStage(
    const char* stage,
    const char* domain,
    std::size_t voices,
    const TimingSnapshot& timing,
    std::uint32_t dry_avg_us,
    std::uint64_t checksum) {
    ESP_LOGI(
        kTag,
        "dsp_stage_bench: stage=%s domain=%s voices=%u blocks=%u min_us=%u avg_us=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u relative_to_dry_milli=%u delta_avg_us=%d checksum=%016llx timing_scope=%s physical_timing_evidence=%u qualification_gate=0",
        stage,
        domain,
        static_cast<unsigned>(voices),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(timing.min_us),
        static_cast<unsigned>(timing.avg_us),
        static_cast<unsigned>(timing.p50_us),
        static_cast<unsigned>(timing.p95_us),
        static_cast<unsigned>(timing.p99_us),
        static_cast<unsigned>(timing.max_us),
        static_cast<unsigned>(timing.deadline_misses),
        static_cast<unsigned>(relativeMilli(timing.avg_us, dry_avg_us)),
        static_cast<int>(deltaUs(timing.avg_us, dry_avg_us)),
        static_cast<unsigned long long>(checksum),
        kTimingScope,
        kPhysicalTimingEvidence);
}

bool runVoiceStages(
    TimingSnapshot* dry_out,
    TimingSnapshot* heavy_out) {
    if (dry_out == nullptr || heavy_out == nullptr) {
        return false;
    }

    void* fixture_storage = heap_caps_malloc(
        sizeof(VoiceBenchFixture),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void* batch_storage = heap_caps_malloc(
        sizeof(gm::ResolvedVoiceBatch),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto* pcm = static_cast<std::int16_t*>(heap_caps_malloc(
        kPcmFrames * sizeof(std::int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* mix = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* reverb = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* chorus = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    bool ok = true;
    VoiceBenchFixture* fixture = nullptr;
    gm::ResolvedVoiceBatch* batch = nullptr;

    if (fixture_storage == nullptr || batch_storage == nullptr || pcm == nullptr ||
        mix == nullptr || reverb == nullptr || chorus == nullptr) {
        ESP_LOGE(
            kTag,
            "dsp_stage_bench: voice scratch allocation failed fixture=%u batch=%u pcm=%u mix=%u reverb=%u chorus=%u free_internal=%u free_psram=%u",
            fixture_storage != nullptr ? 1u : 0u,
            batch_storage != nullptr ? 1u : 0u,
            pcm != nullptr ? 1u : 0u,
            mix != nullptr ? 1u : 0u,
            reverb != nullptr ? 1u : 0u,
            chorus != nullptr ? 1u : 0u,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        ok = false;
    }

    sample::ResidentRecord record{};
    if (ok) {
        fixture = new (fixture_storage) VoiceBenchFixture();
        batch = new (batch_storage) gm::ResolvedVoiceBatch{};
        initializePcm(pcm);

        record.bank_generation = fixture->directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = sample::SampleDescriptor{};
        record.descriptor.id = kSampleId;
        record.descriptor.pcm_bytes =
            static_cast<std::uint32_t>(kPcmFrames * sizeof(std::int16_t));
        record.descriptor.frames = static_cast<std::uint32_t>(kPcmFrames);
        record.descriptor.loop_start = 0;
        record.descriptor.loop_end = static_cast<std::uint32_t>(kPcmFrames);
        record.descriptor.sample_rate = kSampleRate;
        record.descriptor.root_key = 60;

        if (!fixture->directory.publish(kSampleId, &record)) {
            ESP_LOGE(kTag, "dsp_stage_bench: sample publish failed");
            ok = false;
        }
    }

    constexpr std::array<VoiceStage, 8> kStages{
        VoiceStage::Dry,
        VoiceStage::SendBus,
        VoiceStage::StaticFilter,
        VoiceStage::VibratoLfo,
        VoiceStage::ModLfoPitch,
        VoiceStage::ModLfoFull,
        VoiceStage::ModEnvelope,
        VoiceStage::HeavyDsp,
    };

    TimingSnapshot dry{};
    TimingSnapshot heavy{};
    if (ok) {
        ESP_LOGI(
            kTag,
            "dsp_stage_bench: VOICE_START voices=%u frames_per_block=%u warmup_blocks=%u measured_blocks=%u sample_memory=psram timing_scope=%s physical_timing_evidence=%u",
            static_cast<unsigned>(kVoices),
            static_cast<unsigned>(kFramesPerBlock),
            static_cast<unsigned>(kWarmupBlocks),
            static_cast<unsigned>(kMeasuredBlocks),
            kTimingScope,
            kPhysicalTimingEvidence);

        for (const VoiceStage stage : kStages) {
            if (!startStageVoices(stage, *fixture, *batch)) {
                ok = false;
                break;
            }
            std::uint64_t checksum = 0;
            const TimingSnapshot timing = measureVoiceStage(
                stage,
                fixture->voices,
                mix,
                reverb,
                chorus,
                &checksum);
            if (stage == VoiceStage::Dry) {
                dry = timing;
            }
            if (stage == VoiceStage::HeavyDsp) {
                heavy = timing;
            }
            logStage(
                voiceStageName(stage),
                "voice",
                kVoices,
                timing,
                dry.avg_us,
                checksum);
            fixture->voices.allSoundOff();
            vTaskDelay(1);
        }
    }

    if (fixture != nullptr) {
        fixture->voices.allSoundOff();
        fixture->~VoiceBenchFixture();
    }
    if (batch != nullptr) {
        batch->~ResolvedVoiceBatch();
    }
    if (fixture_storage != nullptr) {
        heap_caps_free(fixture_storage);
    }
    if (batch_storage != nullptr) {
        heap_caps_free(batch_storage);
    }
    if (pcm != nullptr) {
        heap_caps_free(pcm);
    }
    if (mix != nullptr) {
        heap_caps_free(mix);
    }
    if (reverb != nullptr) {
        heap_caps_free(reverb);
    }
    if (chorus != nullptr) {
        heap_caps_free(chorus);
    }

    if (ok) {
        *dry_out = dry;
        *heavy_out = heavy;
    }
    return ok;
}

void fillEffectsInput(std::int32_t* reverb, std::int32_t* chorus) {
    for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
        const std::int32_t phase =
            static_cast<std::int32_t>((frame * 37u + 11u) % 97u);
        const std::int32_t base = (phase - 48) * 173;
        reverb[frame * 2u] = base;
        reverb[frame * 2u + 1u] = base / 3;
        chorus[frame * 2u] = -base / 2;
        chorus[frame * 2u + 1u] = base;
    }
}

void processFxStage(
    FxStage stage,
    voice::GlobalEffectsProcessor& effects,
    const std::int32_t* chorus,
    const std::int32_t* reverb,
    std::int32_t* mix) {
    switch (stage) {
        case FxStage::Chorus:
            effects.processChorusAdd(
                chorus,
                mix,
                kFramesPerBlock);
            break;
        case FxStage::Reverb:
            effects.processReverbAdd(
                reverb,
                mix,
                kFramesPerBlock);
            break;
        case FxStage::ChorusReverb:
            effects.processAdd(
                chorus,
                reverb,
                mix,
                kFramesPerBlock);
            break;
    }
}

bool runGlobalFxStages(std::uint32_t dry_avg_us) {
    void* effects_storage = heap_caps_malloc(
        sizeof(voice::GlobalEffectsProcessor),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto* mix = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* reverb = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* chorus = static_cast<std::int32_t*>(heap_caps_malloc(
        kSamplesPerBlock * sizeof(std::int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (effects_storage == nullptr || mix == nullptr ||
        reverb == nullptr || chorus == nullptr) {
        ESP_LOGE(
            kTag,
            "dsp_stage_bench: global FX scratch allocation failed effects=%u mix=%u reverb=%u chorus=%u free_internal=%u",
            effects_storage != nullptr ? 1u : 0u,
            mix != nullptr ? 1u : 0u,
            reverb != nullptr ? 1u : 0u,
            chorus != nullptr ? 1u : 0u,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        if (effects_storage != nullptr) {
            heap_caps_free(effects_storage);
        }
        if (mix != nullptr) {
            heap_caps_free(mix);
        }
        if (reverb != nullptr) {
            heap_caps_free(reverb);
        }
        if (chorus != nullptr) {
            heap_caps_free(chorus);
        }
        return false;
    }

    auto* effects = new (effects_storage) voice::GlobalEffectsProcessor();
    fillEffectsInput(reverb, chorus);

    constexpr std::array<FxStage, 3> kStages{
        FxStage::Chorus,
        FxStage::Reverb,
        FxStage::ChorusReverb,
    };

    ESP_LOGI(
        kTag,
        "dsp_stage_bench: GLOBAL_FX_START frames_per_block=%u warmup_blocks=%u measured_blocks=%u timing_scope=%s physical_timing_evidence=%u",
        static_cast<unsigned>(kFramesPerBlock),
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        kTimingScope,
        kPhysicalTimingEvidence);

    for (const FxStage stage : kStages) {
        effects->reset();
        for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
            std::fill_n(mix, kSamplesPerBlock, 0);
            processFxStage(stage, *effects, chorus, reverb, mix);
        }

        std::array<std::uint32_t, kMeasuredBlocks> timings{};
        std::uint64_t checksum = 1469598103934665603ull;
        for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
            std::fill_n(mix, kSamplesPerBlock, 0);
            const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
            processFxStage(stage, *effects, chorus, reverb, mix);
            const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
            timings[block] = elapsedUs(start_us, end_us);
            checksum = updateChecksum(
                checksum,
                mix,
                kSamplesPerBlock);
        }
        const TimingSnapshot timing = summarize(timings);
        logStage(
            fxStageName(stage),
            "global_fx",
            0,
            timing,
            dry_avg_us,
            checksum);
        vTaskDelay(1);
    }

    effects->~GlobalEffectsProcessor();
    heap_caps_free(effects_storage);
    heap_caps_free(mix);
    heap_caps_free(reverb);
    heap_caps_free(chorus);
    return true;
}

}  // namespace

bool runOptionalDspStageTimingBenchmark() {
    ESP_LOGI(
        kTag,
        "dsp_stage_bench: START purpose=relative_feature_cost absolute_qemu_timing_is_not_hardware_evidence=1");

    TimingSnapshot dry{};
    TimingSnapshot heavy{};
    if (!runVoiceStages(&dry, &heavy)) {
        ESP_LOGE(kTag, "dsp_stage_bench: voice-stage benchmark FAILED");
        return false;
    }
    if (!runGlobalFxStages(dry.avg_us)) {
        ESP_LOGE(kTag, "dsp_stage_bench: global-FX benchmark FAILED");
        return false;
    }

    ESP_LOGI(
        kTag,
        "dsp_stage_bench: PASS dry_avg_us=%u heavy_avg_us=%u heavy_over_dry_milli=%u timing_scope=%s physical_timing_evidence=%u qualification_gate=0",
        static_cast<unsigned>(dry.avg_us),
        static_cast<unsigned>(heavy.avg_us),
        static_cast<unsigned>(relativeMilli(heavy.avg_us, dry.avg_us)),
        kTimingScope,
        kPhysicalTimingEvidence);
    return true;
}

}  // namespace gm_synth::bringup

#else

namespace gm_synth::bringup {

bool runOptionalDspStageTimingBenchmark() {
    return true;
}

}  // namespace gm_synth::bringup

#endif
