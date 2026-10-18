#include "PhysicalMusicStress.h"

#include "platform/EspRealtimeClock.h"

#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";
constexpr sample::SampleId kStressSampleId = 0;
constexpr std::uint32_t kBlockBudgetUs = 1451;
constexpr std::size_t kWarmupBlocks = 16;
constexpr std::size_t kMeasuredBlocks = 128;
constexpr std::int32_t kStressGainQ15 = 8192;
constexpr std::int16_t kStressReverbSend = 500;
constexpr std::int16_t kStressChorusSend = 500;
constexpr std::uint32_t kStressReleaseFrames = 4410;  // 100 ms at 44.1 kHz.
constexpr std::array<std::size_t, 5> kPolyphonySteps{16, 20, 24, 28, 32};
constexpr std::size_t kMinimumQualifiedPolyphony = 24;
constexpr std::size_t kFramesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kFramesPerBlock;
constexpr std::size_t kSamplesPerBlock =
    realtime::ScheduledAudioBlockRenderer::kSamplesPerBlock;

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

voice::VoiceStartParams polyphonyVoiceParams(std::size_t index) {
    voice::VoiceStartParams params{};
    params.sample_id = kStressSampleId;
    params.channel = static_cast<std::uint8_t>(index % 16u);
    params.key = static_cast<std::uint8_t>(48u + (index % 36u));
    params.velocity = 100;
    const std::uint64_t fractional =
        (voice::kPhaseOneQ32 / 97u) * static_cast<std::uint64_t>((index % 7u) + 1u);
    params.phase_increment_q32 = voice::kPhaseOneQ32 + fractional;
    params.loop_mode = voice::LoopMode::Continuous;
    params.gain_left_q15 = kStressGainQ15;
    params.gain_right_q15 = kStressGainQ15;
    params.reverb_send_units = kStressReverbSend;
    params.chorus_send_units = kStressChorusSend;
    params.envelope.attack_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = voice::kQ15Unity;
    params.envelope.release_frames = kStressReleaseFrames;
    return params;
}

std::uint64_t statDelta(std::uint64_t after, std::uint64_t before) {
    return after >= before ? after - before : 0;
}

std::uint32_t statDelta(std::uint32_t after, std::uint32_t before) {
    return after >= before ? after - before : 0;
}

}  // namespace

PhysicalMusicStress::PhysicalMusicStress(sample::PublishedSampleDirectory& directory)
    : directory_(directory) {}

bool PhysicalMusicStress::initialize() {
    if (initialized_) {
        return true;
    }

    for (std::size_t channel = 0; channel < programs_.size(); ++channel) {
        gm::PlaybackZone& zone = zones_[channel];
        zone = gm::PlaybackZone{};
        zone.sample_id = kStressSampleId;
        zone.key_low = 0;
        zone.key_high = 127;
        zone.velocity_low = 1;
        zone.velocity_high = 127;
        zone.root_key = 60;
        zone.key_tracking = true;
        zone.scale_tuning_cents_per_key = 100;
        zone.root_phase_increment_q32 = voice::kPhaseOneQ32;
        zone.loop_mode = voice::LoopMode::Continuous;
        zone.gain_left_q15 = kStressGainQ15;
        zone.gain_right_q15 = kStressGainQ15;
        zone.envelope.attack_frames = 0;
        zone.envelope.decay_frames = 0;
        zone.envelope.sustain_q15 = voice::kQ15Unity;
        zone.envelope.release_frames = kStressReleaseFrames;
        zone.effects_send.enabled = true;
        zone.effects_send.reverb_sf2_units = 300;
        zone.effects_send.chorus_sf2_units = 300;
        zone.effects_send.reverb_cc_amount = 500;
        zone.effects_send.chorus_cc_amount = 500;

        gm::PlaybackProgramSnapshot& program = programs_[channel];
        program = gm::PlaybackProgramSnapshot{};
        program.channel = static_cast<std::uint8_t>(channel);
        program.bank = channel == gm::kGmPercussionChannel
            ? gm::kSf2PercussionBank
            : 0;
        program.program = 0;
        program.token = static_cast<std::uint32_t>(channel + 1u);
        program.bank_generation = directory_.bankGeneration();
        program.zones = &zone;
        program.zone_count = 1;

        if (!program.valid() || !program_exchange_.setInitialActive(&program)) {
            ESP_LOGE(
                kTag,
                "music_stress: initial program publish failed channel=%u bank=%u",
                static_cast<unsigned>(channel),
                static_cast<unsigned>(program.bank));
            return false;
        }
    }

    initialized_ = true;
    ESP_LOGI(
        kTag,
        "music_stress: PROGRAMS_READY melodic_bank=0 percussion_channel=10 percussion_bank=128 release_frames=%u gain_q15=%d",
        static_cast<unsigned>(kStressReleaseFrames),
        static_cast<int>(kStressGainQ15));
    return true;
}

bool PhysicalMusicStress::callback(
    void* context,
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (context == nullptr) {
        return false;
    }
    return static_cast<PhysicalMusicStress*>(context)->applyEvent(event, voices);
}

bool PhysicalMusicStress::applyEvent(
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    const bool ok = program_router_.apply(event, voices);
    timeline_peak_active_voices_ = std::max(
        timeline_peak_active_voices_,
        voices.activeVoiceCount());
    return ok;
}

bool PhysicalMusicStress::runPolyphonySweep(
    voice::VoicePool& voices,
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t* block_start_frame) {
    if (!initialized_ || block_start_frame == nullptr) {
        return false;
    }

    voices.allSoundOff();
    std::array<std::int16_t, kSamplesPerBlock> output{};
    std::array<std::uint32_t, kMeasuredBlocks> timings{};
    std::size_t started = 0;
    bool lower_steps_stable = true;
    max_stable_polyphony_ = 0;

    ESP_LOGI(
        kTag,
        "physical_polyphony: START steps=16,20,24,28,32 warmup_blocks=%u measured_blocks=%u budget_us=%u",
        static_cast<unsigned>(kWarmupBlocks),
        static_cast<unsigned>(kMeasuredBlocks),
        static_cast<unsigned>(kBlockBudgetUs));

    for (const std::size_t target : kPolyphonySteps) {
        while (started < target) {
            if (!voices.start(polyphonyVoiceParams(started))) {
                ESP_LOGE(
                    kTag,
                    "physical_polyphony: voice start failed target=%u started=%u",
                    static_cast<unsigned>(target),
                    static_cast<unsigned>(started));
                voices.allSoundOff();
                return false;
            }
            ++started;
        }

        for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
            renderer.render(*block_start_frame, output.data());
            *block_start_frame += kFramesPerBlock;
        }

        const std::uint64_t clipped_before = renderer.stats().clipped_samples;
        for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
            const std::uint64_t start_us = platform::espRealtimeNowUs(nullptr);
            renderer.render(*block_start_frame, output.data());
            const std::uint64_t end_us = platform::espRealtimeNowUs(nullptr);
            *block_start_frame += kFramesPerBlock;
            timings[block] = elapsedUs(start_us, end_us);
        }

        const TimingSummary timing = summarize(timings);
        const std::uint64_t clipped =
            renderer.stats().clipped_samples - clipped_before;
        const bool stable =
            voices.activeVoiceCount() == target &&
            timing.deadline_misses == 0 &&
            clipped == 0;

        if (lower_steps_stable && stable) {
            max_stable_polyphony_ = target;
        } else {
            lower_steps_stable = false;
        }

        ESP_LOGI(
            kTag,
            "physical_polyphony: voices=%u min_us=%u avg_us=%u p95_us=%u p99_us=%u max_us=%u deadline_misses=%u clipped_samples=%llu stable=%u",
            static_cast<unsigned>(target),
            static_cast<unsigned>(timing.min_us),
            static_cast<unsigned>(timing.avg_us),
            static_cast<unsigned>(timing.p95_us),
            static_cast<unsigned>(timing.p99_us),
            static_cast<unsigned>(timing.max_us),
            static_cast<unsigned>(timing.deadline_misses),
            static_cast<unsigned long long>(clipped),
            stable ? 1u : 0u);
    }

    voices.allSoundOff();
    const bool pass = max_stable_polyphony_ >= kMinimumQualifiedPolyphony;
    ESP_LOGI(
        kTag,
        "physical_polyphony: RESULT max_stable_voices=%u minimum_required=%u hard_cap=%u %s",
        static_cast<unsigned>(max_stable_polyphony_),
        static_cast<unsigned>(kMinimumQualifiedPolyphony),
        static_cast<unsigned>(voice::VoicePool::kHardVoiceCap),
        pass ? "PASS" : "FAIL");
    return pass;
}

bool PhysicalMusicStress::pushEvent(
    realtime::RealtimeEventQueue& queue,
    std::uint64_t frame,
    realtime::EventType type,
    std::uint8_t channel,
    std::uint8_t data1,
    std::uint8_t data2,
    std::uint16_t value) {
    realtime::RealtimeEvent event{};
    event.timestamp_frame = frame;
    event.type = type;
    event.channel = channel;
    event.data1 = data1;
    event.data2 = data2;
    event.value = value;
    if (!queue.tryPush(event)) {
        ESP_LOGE(
            kTag,
            "music_stress: event enqueue failed frame=%llu type=%u channel=%u data1=%u data2=%u value=%u",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned>(type),
            static_cast<unsigned>(channel),
            static_cast<unsigned>(data1),
            static_cast<unsigned>(data2),
            static_cast<unsigned>(value));
        return false;
    }
    ++timeline_events_enqueued_;
    return true;
}

bool PhysicalMusicStress::enqueueTimeline(
    realtime::RealtimeEventQueue& queue,
    voice::VoicePool& voices,
    const realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t block_start_frame,
    std::size_t measured_blocks) {
    if (!initialized_ || measured_blocks == 0 || !queue.empty()) {
        return false;
    }

    timeline_events_enqueued_ = 0;
    timeline_peak_active_voices_ = 0;
    voice_stats_before_timeline_ = voices.stats();
    renderer_stats_before_timeline_ = renderer.stats();
    queue_stats_before_timeline_ = queue.stats();

    const auto phaseFrame = [block_start_frame, measured_blocks](
                                std::size_t numerator,
                                std::size_t denominator) {
        const std::size_t block = std::min<std::size_t>(
            (measured_blocks * numerator) / denominator,
            measured_blocks - 1u);
        return block_start_frame + block * kFramesPerBlock;
    };

    const std::uint64_t p0 = phaseFrame(0, 16);
    if (!pushEvent(queue, p0, realtime::EventType::ControlChange, 0, 91, 100) ||
        !pushEvent(queue, p0, realtime::EventType::ControlChange, 0, 93, 100)) {
        return false;
    }
    for (std::uint8_t key = 48; key < 60; ++key) {
        if (!pushEvent(queue, p0, realtime::EventType::NoteOn, 0, key, 100)) {
            return false;
        }
    }
    constexpr std::array<std::uint8_t, 4> kDrumKeys{35, 38, 42, 46};
    for (const std::uint8_t key : kDrumKeys) {
        if (!pushEvent(
                queue,
                p0,
                realtime::EventType::NoteOn,
                gm::kGmPercussionChannel,
                key,
                110)) {
            return false;
        }
    }

    const std::uint64_t p2 = phaseFrame(2, 16);
    if (!pushEvent(queue, p2, realtime::EventType::ControlChange, 0, 64, 127)) {
        return false;
    }
    for (std::uint8_t key = 48; key < 60; ++key) {
        if (!pushEvent(queue, p2, realtime::EventType::NoteOff, 0, key, 0)) {
            return false;
        }
    }

    const std::uint64_t p3 = phaseFrame(3, 16);
    if (!pushEvent(queue, p3, realtime::EventType::PitchBend, 0, 0, 0, 12288) ||
        !pushEvent(queue, p3, realtime::EventType::ControlChange, 0, 1, 96) ||
        !pushEvent(queue, p3, realtime::EventType::ChannelPressure, 0, 80, 0)) {
        return false;
    }

    const std::uint64_t p4 = phaseFrame(4, 16);
    if (!pushEvent(queue, p4, realtime::EventType::ControlChange, 0, 64, 0)) {
        return false;
    }
    for (const std::uint8_t key : kDrumKeys) {
        if (!pushEvent(
                queue,
                p4,
                realtime::EventType::NoteOff,
                gm::kGmPercussionChannel,
                key,
                0)) {
            return false;
        }
    }

    const std::uint64_t p5 = phaseFrame(5, 16);
    constexpr std::array<std::uint8_t, 4> kPcChannels{0, 1, 2, gm::kGmPercussionChannel};
    constexpr std::array<std::uint8_t, 4> kPcPrograms{40, 48, 24, 0};
    for (std::size_t i = 0; i < kPcChannels.size(); ++i) {
        if (!pushEvent(
                queue,
                p5,
                realtime::EventType::ProgramChange,
                kPcChannels[i],
                kPcPrograms[i],
                0)) {
            return false;
        }
    }

    const std::uint64_t p6 = phaseFrame(6, 16);
    if (!pushEvent(queue, p6, realtime::EventType::ControlChange, 0, 120, 0) ||
        !pushEvent(
            queue,
            p6,
            realtime::EventType::ControlChange,
            gm::kGmPercussionChannel,
            120,
            0)) {
        return false;
    }

    const std::uint64_t p7 = phaseFrame(7, 16);
    for (std::uint8_t channel = 0; channel < 6; ++channel) {
        for (std::uint8_t note = 0; note < 4; ++note) {
            const std::uint8_t key = static_cast<std::uint8_t>(60 + channel * 4 + note);
            if (!pushEvent(
                    queue,
                    p7,
                    realtime::EventType::NoteOn,
                    channel,
                    key,
                    static_cast<std::uint8_t>(90 + note * 8))) {
                return false;
            }
        }
    }

    const std::uint64_t p11 = phaseFrame(11, 16);
    if (!pushEvent(queue, p11, realtime::EventType::ControlChange, 2, 64, 127)) {
        return false;
    }
    for (std::uint8_t channel = 0; channel < 3; ++channel) {
        for (std::uint8_t note = 0; note < 4; ++note) {
            const std::uint8_t key = static_cast<std::uint8_t>(60 + channel * 4 + note);
            if (!pushEvent(queue, p11, realtime::EventType::NoteOff, channel, key, 0)) {
                return false;
            }
        }
    }

    const std::uint64_t p12 = phaseFrame(12, 16);
    if (!pushEvent(queue, p12, realtime::EventType::ControlChange, 2, 64, 0)) {
        return false;
    }
    for (std::uint8_t channel = 0; channel < 4; ++channel) {
        if (!pushEvent(
                queue,
                p12,
                realtime::EventType::ControlChange,
                channel,
                91,
                static_cast<std::uint8_t>(40 + channel * 20)) ||
            !pushEvent(
                queue,
                p12,
                realtime::EventType::ControlChange,
                channel,
                93,
                static_cast<std::uint8_t>(100 - channel * 15))) {
            return false;
        }
    }

    const std::uint64_t p13 = phaseFrame(13, 16);
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        const std::uint8_t program = static_cast<std::uint8_t>((channel * 7u) & 0x7fu);
        if (!pushEvent(
                queue,
                p13,
                realtime::EventType::ProgramChange,
                channel,
                program,
                0)) {
            return false;
        }
    }

    const std::uint64_t p27 = phaseFrame(27, 32);
    constexpr std::array<std::uint8_t, 4> kBurstPrograms{12, 27, 54, 81};
    for (const std::uint8_t program : kBurstPrograms) {
        if (!pushEvent(
                queue,
                p27,
                realtime::EventType::ProgramChange,
                0,
                program,
                0)) {
            return false;
        }
    }

    const std::uint64_t p14 = phaseFrame(14, 16);
    for (std::uint8_t channel = 0; channel < 6; ++channel) {
        if (!pushEvent(
                queue,
                p14,
                realtime::EventType::ControlChange,
                channel,
                123,
                0)) {
            return false;
        }
    }

    const std::uint64_t p15 = phaseFrame(15, 16);
    if (!pushEvent(queue, p15, realtime::EventType::GmReset, 0, 0, 0)) {
        return false;
    }

    ESP_LOGI(
        kTag,
        "music_stress: TIMELINE_ARMED events=%u measured_blocks=%u duration_frames=%llu includes=sustain,release,percussion,pitch,cc,program_handoff,gm_reset",
        static_cast<unsigned>(timeline_events_enqueued_),
        static_cast<unsigned>(measured_blocks),
        static_cast<unsigned long long>(measured_blocks * kFramesPerBlock));
    return true;
}

bool PhysicalMusicStress::validateAndLog(
    realtime::RealtimeEventQueue& queue,
    const voice::VoicePool& voices,
    const realtime::ScheduledAudioBlockRenderer& renderer) {
    const auto event_stats = event_applier_.stats();
    const auto router_stats = program_router_.stats();
    const auto handoff_before_take = program_handoff_.stats();
    const auto voice_stats = voices.stats();
    const auto renderer_stats = renderer.stats();
    const auto queue_stats = queue.stats();

    const std::uint64_t render_events_attempted = statDelta(
        renderer_stats.events_attempted,
        renderer_stats_before_timeline_.events_attempted);
    const std::uint64_t render_events_applied = statDelta(
        renderer_stats.events_applied,
        renderer_stats_before_timeline_.events_applied);
    const std::uint64_t render_events_rejected = statDelta(
        renderer_stats.events_rejected,
        renderer_stats_before_timeline_.events_rejected);
    const std::uint64_t sustain_deferred = statDelta(
        voice_stats.sustain_deferred_note_offs,
        voice_stats_before_timeline_.sustain_deferred_note_offs);
    const std::uint64_t sustain_released = statDelta(
        voice_stats.sustain_release_voices,
        voice_stats_before_timeline_.sustain_release_voices);
    const std::uint32_t queue_rejected_full = statDelta(
        queue_stats.rejected_full,
        queue_stats_before_timeline_.rejected_full);
    const std::uint32_t queue_rejected_order = statDelta(
        queue_stats.rejected_out_of_order,
        queue_stats_before_timeline_.rejected_out_of_order);

    std::size_t taken_channels = 0;
    bool latest_programs_ok = true;
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        gm::ProgramChangeRequest request{};
        if (!program_handoff_.takeLatest(channel, &request)) {
            latest_programs_ok = false;
            continue;
        }
        ++taken_channels;
        const std::uint16_t expected_bank = channel == gm::kGmPercussionChannel
            ? gm::kSf2PercussionBank
            : 0;
        if (request.channel != channel ||
            request.bank != expected_bank ||
            request.program != 0 ||
            request.token == 0) {
            latest_programs_ok = false;
        }
    }
    const auto handoff_after_take = program_handoff_.stats();

    ESP_LOGI(
        kTag,
        "music_stress: EVENTS queued=%u render_attempted=%llu render_applied=%llu render_rejected=%llu note_on=%llu note_off=%llu cc=%llu pitch_bend=%llu pressure=%llu all_notes_off=%llu all_sound_off=%llu",
        static_cast<unsigned>(timeline_events_enqueued_),
        static_cast<unsigned long long>(render_events_attempted),
        static_cast<unsigned long long>(render_events_applied),
        static_cast<unsigned long long>(render_events_rejected),
        static_cast<unsigned long long>(event_stats.note_on_events),
        static_cast<unsigned long long>(event_stats.note_off_events),
        static_cast<unsigned long long>(event_stats.control_change_events),
        static_cast<unsigned long long>(event_stats.pitch_bend_events),
        static_cast<unsigned long long>(event_stats.channel_pressure_events),
        static_cast<unsigned long long>(event_stats.all_notes_off_events),
        static_cast<unsigned long long>(event_stats.all_sound_off_events));

    ESP_LOGI(
        kTag,
        "music_stress: VOICES peak_active=%u sustain_deferred=%llu sustain_released=%llu voices_resolved=%llu voices_started=%llu start_failures=%llu percussion_unavailable=%llu no_matching_zones=%llu resolve_overflows=%llu",
        static_cast<unsigned>(timeline_peak_active_voices_),
        static_cast<unsigned long long>(sustain_deferred),
        static_cast<unsigned long long>(sustain_released),
        static_cast<unsigned long long>(event_stats.voices_resolved),
        static_cast<unsigned long long>(event_stats.voices_started),
        static_cast<unsigned long long>(event_stats.voice_start_failures),
        static_cast<unsigned long long>(event_stats.percussion_program_unavailable),
        static_cast<unsigned long long>(event_stats.no_matching_zones),
        static_cast<unsigned long long>(event_stats.resolve_overflows));

    ESP_LOGI(
        kTag,
        "music_stress: PROGRAM_HANDOFF program_events=%llu gm_reset_events=%llu gm_reset_requests=%llu requests_published=%llu requests_rejected=%llu handoff_published=%u superseded=%u taken_channels=%u handoff_taken=%u read_retries=%u latest_programs_ok=%u",
        static_cast<unsigned long long>(router_stats.program_change_events),
        static_cast<unsigned long long>(router_stats.gm_reset_events),
        static_cast<unsigned long long>(router_stats.gm_reset_program_requests),
        static_cast<unsigned long long>(router_stats.requests_published),
        static_cast<unsigned long long>(router_stats.requests_rejected),
        static_cast<unsigned>(handoff_before_take.published),
        static_cast<unsigned>(handoff_before_take.superseded),
        static_cast<unsigned>(taken_channels),
        static_cast<unsigned>(handoff_after_take.taken),
        static_cast<unsigned>(handoff_after_take.read_retries),
        latest_programs_ok ? 1u : 0u);

    const bool pass =
        max_stable_polyphony_ >= kMinimumQualifiedPolyphony &&
        timeline_events_enqueued_ != 0 &&
        render_events_attempted == timeline_events_enqueued_ &&
        render_events_applied == timeline_events_enqueued_ &&
        render_events_rejected == 0 &&
        queue.empty() &&
        queue_rejected_full == 0 &&
        queue_rejected_order == 0 &&
        timeline_peak_active_voices_ >= 24 &&
        sustain_deferred >= 16 &&
        sustain_released >= 16 &&
        event_stats.note_on_events >= 40 &&
        event_stats.note_off_events >= 28 &&
        event_stats.pitch_bend_events >= 1 &&
        event_stats.channel_pressure_events >= 1 &&
        event_stats.voice_start_failures == 0 &&
        event_stats.percussion_program_unavailable == 0 &&
        event_stats.resolve_overflows == 0 &&
        router_stats.program_change_events >= 24 &&
        router_stats.gm_reset_events == 1 &&
        router_stats.gm_reset_program_requests == 16 &&
        router_stats.requests_rejected == 0 &&
        handoff_before_take.published >= 40 &&
        handoff_before_take.superseded > 0 &&
        taken_channels == 16 &&
        latest_programs_ok;

    ESP_LOGI(
        kTag,
        "music_stress: RESULT max_stable_polyphony=%u peak_dynamic_voices=%u queue_remaining=%u queue_rejected_full=%u queue_rejected_order=%u %s",
        static_cast<unsigned>(max_stable_polyphony_),
        static_cast<unsigned>(timeline_peak_active_voices_),
        static_cast<unsigned>(queue.size()),
        static_cast<unsigned>(queue_rejected_full),
        static_cast<unsigned>(queue_rejected_order),
        pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace gm_synth::bringup
