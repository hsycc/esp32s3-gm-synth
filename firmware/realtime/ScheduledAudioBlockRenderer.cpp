#include "ScheduledAudioBlockRenderer.h"

#include "util/SaturatingCounter.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::realtime {
namespace {

void incrementProfileCount(std::uint8_t& value) {
    if (value != std::numeric_limits<std::uint8_t>::max()) {
        ++value;
    }
}

}  // namespace

std::int16_t ScheduledAudioBlockRenderer::saturateToI16(
    std::int32_t sample,
    bool* clipped) {
    if (sample > std::numeric_limits<std::int16_t>::max()) {
        if (clipped != nullptr) {
            *clipped = true;
        }
        return std::numeric_limits<std::int16_t>::max();
    }
    if (sample < std::numeric_limits<std::int16_t>::min()) {
        if (clipped != nullptr) {
            *clipped = true;
        }
        return std::numeric_limits<std::int16_t>::min();
    }
    return static_cast<std::int16_t>(sample);
}

std::uint8_t ScheduledAudioBlockRenderer::saturatingProfileCount(std::size_t value) {
    return static_cast<std::uint8_t>(std::min<std::size_t>(
        value,
        std::numeric_limits<std::uint8_t>::max()));
}

bool ScheduledAudioBlockRenderer::inspectProfileVoice(
    void* context,
    voice::Voice& voice) {
    auto* snapshot = static_cast<ScheduledWorstBlockVoiceContext*>(context);
    if (snapshot == nullptr) {
        return false;
    }

    incrementProfileCount(snapshot->active);
    snapshot->channel_mask = static_cast<std::uint16_t>(
        snapshot->channel_mask | (std::uint16_t{1} << voice.channel()));
    if (voice.releasing()) {
        incrementProfileCount(snapshot->releasing);
    }
    if (voice.lowpassEnabled()) {
        incrementProfileCount(snapshot->lowpass);
    }
    if (voice.vibratoLfoConfig().enabled) {
        incrementProfileCount(snapshot->vibrato);
    }
    if (voice.modLfoConfig().enabled) {
        incrementProfileCount(snapshot->mod_lfo);
    }
    if (voice.modEnvelopeConfig().enabled) {
        incrementProfileCount(snapshot->mod_env);
    }
    if (voice.reverbSendUnits() > 0 || voice.chorusSendUnits() > 0) {
        incrementProfileCount(snapshot->fx_send);
    }
    if (voice.realtimeControl().enabled) {
        incrementProfileCount(snapshot->realtime_control);
    }

    return false;
}

ScheduledWorstBlockVoiceContext ScheduledAudioBlockRenderer::captureProfileVoices() {
    ScheduledWorstBlockVoiceContext snapshot{};
#if defined(ESP_PLATFORM)
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        (void)voices_.updateActiveVoices(
            channel,
            &ScheduledAudioBlockRenderer::inspectProfileVoice,
            &snapshot);
    }
    snapshot.fast_release_tails = saturatingProfileCount(voices_.activeFastReleaseTailCount());
    snapshot.steal_tails = saturatingProfileCount(voices_.activeStealTailCount());
    snapshot.exclusive_tails = saturatingProfileCount(voices_.activeExclusiveTailCount());
#endif
    return snapshot;
}

void ScheduledAudioBlockRenderer::beginWorstBlockProfiling(
    std::uint32_t deadline_us,
    std::uint64_t first_block_start_frame) {
    worst_profile_ = ScheduledWorstBlockProfile{};
    worst_profile_.deadline_us = deadline_us;
    worst_profile_first_frame_ = first_block_start_frame;
    worst_profile_sustain_mask_ = 0;
    last_profiled_render_us_ = 0;
    worst_profile_enabled_ = deadline_us != 0u;
}

void ScheduledAudioBlockRenderer::insertWorstBlock(
    ScheduledWorstBlockEntry entry) {
    std::size_t insert_at = 0;
    while (insert_at < worst_profile_.retained &&
           worst_profile_.slowest[insert_at].render_us >= entry.render_us) {
        ++insert_at;
    }
    if (insert_at >= worst_profile_.slowest.size()) {
        return;
    }

    const std::size_t previous_retained = worst_profile_.retained;
    worst_profile_.retained = std::min<std::size_t>(
        worst_profile_.retained + 1u,
        worst_profile_.slowest.size());
    for (std::size_t index = worst_profile_.retained - 1u;
         index > insert_at;
         --index) {
        if (index - 1u < previous_retained) {
            worst_profile_.slowest[index] = worst_profile_.slowest[index - 1u];
        }
    }
    worst_profile_.slowest[insert_at] = entry;
}

void ScheduledAudioBlockRenderer::recordWorstBlock(
    std::uint64_t block_start_frame,
    std::uint32_t render_us,
    const BlockEventBatch& batch) {
    // Profiling may be armed before the I2S helper runs its warmup. Keep those
    // warmup blocks completely outside the measured Top-N/candidate window so
    // the profiler and realtime-health counters describe the same block range.
    if (block_start_frame < worst_profile_first_frame_) {
        return;
    }

    ScheduledWorstBlockEventContext event_context{};
    for (std::size_t index = 0; index < batch.count; ++index) {
        const RealtimeEvent& event = batch.events[index].event;
        incrementProfileCount(event_context.total);
        if (event.channel < 16u) {
            event_context.event_channel_mask = static_cast<std::uint16_t>(
                event_context.event_channel_mask |
                (std::uint16_t{1} << event.channel));
        }

        switch (event.type) {
            case EventType::NoteOn:
                incrementProfileCount(event_context.note_on);
                break;
            case EventType::NoteOff:
                incrementProfileCount(event_context.note_off);
                break;
            case EventType::ControlChange:
                incrementProfileCount(event_context.control_change);
                if (event.channel < 16u && event.data1 == 64u) {
                    const std::uint16_t mask = static_cast<std::uint16_t>(
                        std::uint16_t{1} << event.channel);
                    if (event.data2 >= 64u) {
                        worst_profile_sustain_mask_ = static_cast<std::uint16_t>(
                            worst_profile_sustain_mask_ | mask);
                    } else {
                        worst_profile_sustain_mask_ = static_cast<std::uint16_t>(
                            worst_profile_sustain_mask_ & ~mask);
                    }
                }
                break;
            case EventType::ProgramChange:
                incrementProfileCount(event_context.program_change);
                break;
            case EventType::PitchBend:
                incrementProfileCount(event_context.pitch_bend);
                break;
            case EventType::ChannelPressure:
                incrementProfileCount(event_context.channel_pressure);
                break;
            case EventType::GmReset:
                incrementProfileCount(event_context.gm_reset);
                worst_profile_sustain_mask_ = 0;
                break;
        }
    }
    event_context.sustain_channel_mask = worst_profile_sustain_mask_;

    if (render_us <= worst_profile_.deadline_us) {
        return;
    }

    ++worst_profile_.deadline_candidates;
    if (worst_profile_.retained == worst_profile_.slowest.size() &&
        render_us <= worst_profile_.slowest[worst_profile_.retained - 1u].render_us) {
        return;
    }

    ScheduledWorstBlockEntry entry{};
    entry.valid = true;
    entry.block_start_frame = block_start_frame;
    entry.stream_block = static_cast<std::size_t>(
        (block_start_frame - worst_profile_first_frame_) / kFramesPerBlock);
    entry.render_us = render_us;
    entry.events = event_context;
    entry.voices = captureProfileVoices();
    insertWorstBlock(entry);
}

void ScheduledAudioBlockRenderer::renderSegment(
    std::size_t start_frame,
    std::size_t frame_count) {
    if (frame_count == 0) {
        return;
    }
    const std::size_t sample_offset = start_frame * kChannels;
    voices_.renderAddWithEffects(
        mix_.data() + sample_offset,
        reverb_send_.data() + sample_offset,
        chorus_send_.data() + sample_offset,
        frame_count);
    util::saturatingIncrement(stats_.render_segments);
}

void ScheduledAudioBlockRenderer::finalize(std::int16_t* interleaved_stereo) {
    effects_.processAdd(
        chorus_send_.data(),
        reverb_send_.data(),
        mix_.data(),
        kFramesPerBlock);

    for (std::size_t index = 0; index < mix_.size(); ++index) {
        const std::int32_t value = mix_[index];
        const std::int64_t magnitude = value >= 0
            ? static_cast<std::int64_t>(value)
            : -static_cast<std::int64_t>(value);
        stats_.peak_abs_accumulator = std::max(
            stats_.peak_abs_accumulator,
            static_cast<std::int32_t>(std::min<std::int64_t>(
                magnitude,
                std::numeric_limits<std::int32_t>::max())));

        bool clipped = false;
        interleaved_stereo[index] = saturateToI16(value, &clipped);
        if (clipped) {
            util::saturatingIncrement(stats_.clipped_samples);
        }
    }
}

void ScheduledAudioBlockRenderer::render(
    std::uint64_t block_start_frame,
    std::int16_t* interleaved_stereo) {
    if (interleaved_stereo == nullptr) {
        return;
    }

#if defined(ESP_PLATFORM)
    const std::int64_t worst_profile_start_us = worst_profile_enabled_
        ? esp_timer_get_time()
        : 0;
#endif

    mix_.fill(0);
    reverb_send_.fill(0);
    chorus_send_.fill(0);
    const BlockEventBatch& batch = scheduler_.prepareBlock(
        block_start_frame,
        static_cast<std::uint16_t>(kFramesPerBlock));

    std::size_t cursor = 0;
    std::size_t event_index = 0;
    while (event_index < batch.count) {
        const std::size_t offset = std::min<std::size_t>(
            batch.events[event_index].frame_offset,
            kFramesPerBlock);
        if (offset > cursor) {
            renderSegment(cursor, offset - cursor);
            cursor = offset;
        }

        while (event_index < batch.count &&
               batch.events[event_index].frame_offset == offset) {
            util::saturatingIncrement(stats_.events_attempted);
            const bool applied = apply_event_ != nullptr &&
                apply_event_(apply_context_, batch.events[event_index].event, voices_);
            if (applied) {
                util::saturatingIncrement(stats_.events_applied);
            } else {
                util::saturatingIncrement(stats_.events_rejected);
            }
            ++event_index;
        }
    }

    if (cursor < kFramesPerBlock) {
        renderSegment(cursor, kFramesPerBlock - cursor);
    }

    finalize(interleaved_stereo);
    util::saturatingIncrement(stats_.blocks_rendered);
    util::saturatingAdd(stats_.output_frames, static_cast<std::uint64_t>(kFramesPerBlock));

#if defined(ESP_PLATFORM)
    if (worst_profile_enabled_) {
        const std::int64_t end_us = esp_timer_get_time();
        const std::uint64_t elapsed = end_us >= worst_profile_start_us
            ? static_cast<std::uint64_t>(end_us - worst_profile_start_us)
            : 0u;
        const std::uint32_t render_us = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                elapsed,
                std::numeric_limits<std::uint32_t>::max()));
        last_profiled_render_us_ = render_us;
        recordWorstBlock(block_start_frame, render_us, batch);
    }
#endif
}

}  // namespace gm_synth::realtime
