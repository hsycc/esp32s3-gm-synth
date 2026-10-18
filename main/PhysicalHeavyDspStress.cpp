#include "PhysicalHeavyDspStress.h"

#include "gm/MidiChannelState.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "platform/EspRealtimeClock.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

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
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kWarmupBlocks = 16;
constexpr std::size_t kMeasuredBlocks = 128;
constexpr std::size_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
constexpr std::size_t kSamplesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock;
constexpr std::array<std::size_t, 10> kSteps{1, 2, 4, 8, 12, 16, 20, 24, 28, 32};

constexpr std::uint32_t kFailureNullBlockStart = 1u << 0u;
constexpr std::uint32_t kFailureInvalidFixture = 1u << 1u;
constexpr std::uint32_t kFailureScratchAllocation = 1u << 2u;
constexpr std::uint32_t kFailureResolveStatus = 1u << 3u;
constexpr std::uint32_t kFailureResolveCount = 1u << 4u;
constexpr std::uint32_t kFailureLowpass = 1u << 5u;
constexpr std::uint32_t kFailureDynamicLowpass = 1u << 6u;
constexpr std::uint32_t kFailureVibrato = 1u << 7u;
constexpr std::uint32_t kFailureModLfo = 1u << 8u;
constexpr std::uint32_t kFailureModEnvelope = 1u << 9u;
constexpr std::uint32_t kFailureVoiceStart = 1u << 10u;
constexpr std::uint32_t kFailureActiveVoiceCount = 1u << 11u;
constexpr std::uint32_t kFailureDeadline = 1u << 12u;
constexpr std::uint32_t kFailureClipping = 1u << 13u;

struct TimingSummary {
    std::uint32_t min_us = 0;
    std::uint32_t avg_us = 0;
    std::uint32_t p95_us = 0;
    std::uint32_t p99_us = 0;
    std::uint32_t max_us = 0;
    std::uint32_t deadline_misses = 0;
};

std::uint32_t elapsedUs(std::uint64_t start_us, std::uint64_t end_us) {
    if (end_us < start_us) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_us - start_us,
        std::numeric_limits<std::uint32_t>::max()));
}

TimingSummary summarize(const std::array<std::uint32_t, kMeasuredBlocks>& values) {
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

    TimingSummary result{};
    result.min_us = sorted.front();
    result.avg_us = static_cast<std::uint32_t>(sum / values.size());
    result.p95_us = percentile(95);
    result.p99_us = percentile(99);
    result.max_us = sorted.back();
    result.deadline_misses = misses;
    return result;
}

std::uint64_t updateChecksum(
    std::uint64_t checksum,
    const std::array<std::int16_t, kSamplesPerBlock>& output) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (const std::int16_t sample : output) {
        checksum ^= static_cast<std::uint16_t>(sample);
        checksum *= kFnvPrime;
    }
    return checksum;
}

void recordFailure(
    PhysicalHeavyDspStressResult* result,
    std::size_t voices,
    std::size_t active_voices,
    std::uint32_t deadline_misses,
    std::uint64_t clipped_samples,
    std::uint32_t flags) {
    if (result == nullptr || flags == 0u) {
        return;
    }
    result->failure_flags |= flags;
    if (result->first_failed_voices != 0u) {
        return;
    }
    result->first_failed_voices = voices;
    result->first_failed_active_voices = active_voices;
    result->first_failed_deadline_misses = deadline_misses;
    result->first_failed_clipped_samples = clipped_samples;
}

void logResult(const PhysicalHeavyDspStressResult& result) {
    ESP_LOGI(
        kTag,
        "physical_heavy_dsp: RESULT max_stable_voices=%u minimum_required=%u first_failed_voices=%u first_failed_active=%u first_failed_deadline_misses=%u first_failed_clipped_samples=%llu failure_flags=0x%08x hard_cap=%u %s",
        static_cast<unsigned>(result.max_stable_voices),
        static_cast<unsigned>(result.minimum_required_voices),
        static_cast<unsigned>(result.first_failed_voices),
        static_cast<unsigned>(result.first_failed_active_voices),
        static_cast<unsigned>(result.first_failed_deadline_misses),
        static_cast<unsigned long long>(result.first_failed_clipped_samples),
        static_cast<unsigned>(result.failure_flags),
        static_cast<unsigned>(voice::VoicePool::kHardVoiceCap),
        result.pass() ? "PASS" : "FAIL");
}

gm::PlaybackZone makeHeavyZone() {
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

    // 24 dB of static attenuation keeps a 32-Voice coherent synthetic fixture
    // away from clipping while preserving the exact DSP work per Voice.
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

    // Based on the already-qualified filter/resonance runtime test shape.
    zone.note_on_filter.enabled = true;
    zone.note_on_filter.initial_cutoff_cents = 8000;
    zone.note_on_filter.velocity_to_cutoff_cents = 0;
    zone.note_on_filter.resonance_cb = 100;

    // Realistic-rate Vibrato-LFO with live controller routes retained in the
    // resolved realtime-control state.
    zone.vibrato_lfo.enabled = true;
    zone.vibrato_lfo.delay_frames = 0;
    zone.vibrato_lfo.phase_step_q32 = std::uint32_t{1} << 19u;
    zone.vibrato_lfo.static_pitch_depth_cents = 50;
    zone.vibrato_lfo.channel_pressure_to_pitch_cents = 50;
    zone.vibrato_lfo.modulation_wheel_to_pitch_cents = 50;

    // One Mod-LFO simultaneously drives pitch, dynamic filter and nonlinear
    // gain. This deliberately prevents the simple continuous-Voice fast path.
    zone.mod_lfo.enabled = true;
    zone.mod_lfo.delay_frames = 0;
    zone.mod_lfo.phase_step_q32 = std::uint32_t{1} << 20u;
    zone.mod_lfo.pitch_depth_cents = 100;
    zone.mod_lfo.filter_depth_cents = 1200;
    zone.mod_lfo.volume_depth_cb = 120;

    // Mod-Envelope contributes another independent pitch/filter source so the
    // combined DynamicLowpassTable interpolation path is active.
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.envelope.attack_frames = 128;
    zone.mod_envelope.envelope.hold_frames = 64;
    zone.mod_envelope.envelope.decay_frames = 256;
    zone.mod_envelope.envelope.sustain_q15 = voice::kQ15Unity / 2;
    zone.mod_envelope.envelope.release_frames = 4410;
    zone.mod_envelope.pitch_depth_cents = 600;
    zone.mod_envelope.filter_depth_cents = 600;

    zone.effects_send.enabled = true;
    zone.effects_send.reverb_sf2_units = 500;
    zone.effects_send.chorus_sf2_units = 500;
    zone.effects_send.reverb_cc_amount = 0;
    zone.effects_send.chorus_cc_amount = 0;
    return zone;
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

}  // namespace

PhysicalHeavyDspStressResult runPhysicalHeavyDspStress(
    sample::PublishedSampleDirectory& directory,
    voice::VoicePool& voices,
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t* block_start_frame) {
    PhysicalHeavyDspStressResult result{};
    if (block_start_frame == nullptr) {
        result.failure_flags |= kFailureNullBlockStart;
        ESP_LOGE(kTag, "physical_heavy_dsp: null block_start_frame");
        logResult(result);
        return result;
    }

    const gm::PlaybackZone zone = makeHeavyZone();
    const gm::PlaybackProgramSnapshot program = makeProgram(
        &zone,
        directory.bankGeneration());
    if (!zone.valid() || !program.valid()) {
        result.failure_flags |= kFailureInvalidFixture;
        ESP_LOGE(
            kTag,
            "physical_heavy_dsp: invalid synthetic heavy zone/program zone_valid=%u program_valid=%u bank_generation=%u",
            zone.valid() ? 1u : 0u,
            program.valid() ? 1u : 0u,
            static_cast<unsigned>(directory.bankGeneration()));
        logResult(result);
        return result;
    }

    gm::MidiChannelState channel_state{};
    channel_state.resetAll();
    channel_state.volume = 127;
    channel_state.expression = 127;
    channel_state.pan = 64;

    // ResolvedVoiceBatch contains storage for the hard-cap number of complete
    // VoiceStartParams and is far too large for app_main's stack. Resolution is
    // performed only before each measured polyphony step, never in the timed
    // render loop, so PSRAM is the correct place for this diagnostic scratch.
    void* batch_storage = heap_caps_malloc(
        sizeof(gm::ResolvedVoiceBatch),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (batch_storage == nullptr) {
        result.failure_flags |= kFailureScratchAllocation;
        ESP_LOGE(
            kTag,
            "physical_heavy_dsp: PSRAM scratch allocation failed bytes=%u free_psram=%u",
            static_cast<unsigned>(sizeof(gm::ResolvedVoiceBatch)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        logResult(result);
        return result;
    }
    auto* batch = new (batch_storage) gm::ResolvedVoiceBatch{};
    ESP_LOGI(
        kTag,
        "physical_heavy_dsp: RESOLVE_SCRATCH memory=psram bytes=%u",
        static_cast<unsigned>(sizeof(gm::ResolvedVoiceBatch)));

    gm::ZoneResolver resolver;
    voices.allSoundOff();
    std::array<std::int16_t, kSamplesPerBlock> output{};
    std::array<std::uint32_t, kMeasuredBlocks> timings{};
    std::size_t started = 0;
    bool lower_steps_stable = true;

    ESP_LOGI(
        kTag,
        "physical_heavy_dsp: START steps=1,2,4,8,12,16,20,24,28,32 warmup_blocks=%u measured_blocks=%u budget_us=%u features=resonant_filter,vibrato_lfo,mod_lfo_pitch_filter_volume,mod_env_pitch_filter,full_fx",
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(kBlockBudgetUs));

    for (const std::size_t target : kSteps) {
        while (started < target) {
            batch->clear();
            const std::uint8_t key = static_cast<std::uint8_t>(48u + (started % 36u));
            const gm::ZoneResolveStatus status = resolver.resolveNoteOn(
                program,
                channel_state,
                0,
                key,
                100,
                static_cast<std::uint32_t>(started + 1u),
                batch);

            std::uint32_t setup_failure_flags = 0;
            if (status != gm::ZoneResolveStatus::Ok) {
                setup_failure_flags |= kFailureResolveStatus;
            }
            const bool resolved_one = batch->count == 1u;
            if (!resolved_one) {
                setup_failure_flags |= kFailureResolveCount;
            }

            bool lowpass = false;
            bool dynamic_lowpass = false;
            bool vibrato = false;
            bool mod_lfo = false;
            bool mod_envelope = false;
            if (resolved_one) {
                lowpass = batch->voices[0].lowpass.enabled;
                dynamic_lowpass = batch->voices[0].dynamic_lowpass_table.enabled;
                vibrato = batch->voices[0].vibrato_lfo.enabled;
                mod_lfo = batch->voices[0].mod_lfo.enabled;
                mod_envelope = batch->voices[0].mod_envelope.enabled;
                if (!lowpass) {
                    setup_failure_flags |= kFailureLowpass;
                }
                if (!dynamic_lowpass) {
                    setup_failure_flags |= kFailureDynamicLowpass;
                }
                if (!vibrato) {
                    setup_failure_flags |= kFailureVibrato;
                }
                if (!mod_lfo) {
                    setup_failure_flags |= kFailureModLfo;
                }
                if (!mod_envelope) {
                    setup_failure_flags |= kFailureModEnvelope;
                }
            }

            bool voice_start_ok = false;
            if (setup_failure_flags == 0u) {
                voice_start_ok = voices.start(batch->voices[0]);
                if (!voice_start_ok) {
                    setup_failure_flags |= kFailureVoiceStart;
                }
            }

            if (setup_failure_flags != 0u) {
                recordFailure(
                    &result,
                    target,
                    voices.activeVoiceCount(),
                    0,
                    0,
                    setup_failure_flags);
                ESP_LOGE(
                    kTag,
                    "physical_heavy_dsp: FIXTURE_FAIL target=%u started=%u status=%u resolved=%u lowpass=%u dynamic_lowpass=%u vibrato=%u mod_lfo=%u mod_env=%u voice_start=%u active=%u failure_flags=0x%08x",
                    static_cast<unsigned>(target),
                    static_cast<unsigned>(started),
                    static_cast<unsigned>(status),
                    static_cast<unsigned>(batch->count),
                    lowpass ? 1u : 0u,
                    dynamic_lowpass ? 1u : 0u,
                    vibrato ? 1u : 0u,
                    mod_lfo ? 1u : 0u,
                    mod_envelope ? 1u : 0u,
                    voice_start_ok ? 1u : 0u,
                    static_cast<unsigned>(voices.activeVoiceCount()),
                    static_cast<unsigned>(setup_failure_flags));
                voices.allSoundOff();
                batch->~ResolvedVoiceBatch();
                heap_caps_free(batch_storage);
                logResult(result);
                return result;
            }
            ++started;
        }

        for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
            renderer.render(*block_start_frame, output.data());
            *block_start_frame += kFramesPerBlock;
        }

        const std::uint64_t clipped_before = renderer.stats().clipped_samples;
        std::uint64_t checksum = 1469598103934665603ull;
        for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
            const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
            renderer.render(*block_start_frame, output.data());
            const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
            *block_start_frame += kFramesPerBlock;
            timings[block] = elapsedUs(start_us, end_us);
            checksum = updateChecksum(checksum, output);
        }

        const TimingSummary timing = summarize(timings);
        const std::uint64_t clipped = renderer.stats().clipped_samples - clipped_before;
        const std::size_t active_voices = voices.activeVoiceCount();
        std::uint32_t step_failure_flags = 0;
        if (active_voices != target) {
            step_failure_flags |= kFailureActiveVoiceCount;
        }
        if (timing.deadline_misses != 0u) {
            step_failure_flags |= kFailureDeadline;
        }
        if (clipped != 0u) {
            step_failure_flags |= kFailureClipping;
        }
        const bool stable = step_failure_flags == 0u;

        if (stable && lower_steps_stable) {
            result.max_stable_voices = target;
        } else {
            if (!stable) {
                recordFailure(
                    &result,
                    target,
                    active_voices,
                    timing.deadline_misses,
                    clipped,
                    step_failure_flags);
            }
            lower_steps_stable = false;
        }

        ESP_LOGI(
            kTag,
            "physical_heavy_dsp: voices=%u active_voices=%u min_us=%u avg_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u clipped_samples=%llu checksum=%016llx failure_flags=0x%08x stable=%u",
            static_cast<unsigned>(target),
            static_cast<unsigned>(active_voices),
            static_cast<unsigned>(timing.min_us),
            static_cast<unsigned>(timing.avg_us),
            static_cast<unsigned>(timing.p95_us),
            static_cast<unsigned>(timing.p99_us),
            static_cast<unsigned>(timing.max_us),
            static_cast<unsigned>(timing.deadline_misses),
            static_cast<unsigned long long>(clipped),
            static_cast<unsigned long long>(checksum),
            static_cast<unsigned>(step_failure_flags),
            stable ? 1u : 0u);
    }

    voices.allSoundOff();
    batch->~ResolvedVoiceBatch();
    heap_caps_free(batch_storage);
    logResult(result);
    return result;
}

}  // namespace gm_synth::bringup
