#include "I2sWorstBlockProfiler.h"

#include "esp_log.h"

namespace gm_synth::bringup {
namespace {

constexpr char kTag[] = "gm_synth";

}  // namespace

void logI2sWorstBlockProfile(
    const realtime::ScheduledWorstBlockProfile& profile) {
    const std::uint32_t slowest_us = profile.retained == 0u
        ? 0u
        : profile.slowest[0].render_us;
    const std::uint32_t floor_us = profile.retained == 0u
        ? 0u
        : profile.slowest[profile.retained - 1u].render_us;

    ESP_LOGI(
        kTag,
        "i2s_worst: SUMMARY deadline_us=%u deadline_candidates=%u retained=%u slowest_render_us=%u retained_floor_us=%u",
        static_cast<unsigned>(profile.deadline_us),
        static_cast<unsigned>(profile.deadline_candidates),
        static_cast<unsigned>(profile.retained),
        static_cast<unsigned>(slowest_us),
        static_cast<unsigned>(floor_us));

    for (std::size_t index = 0; index < profile.retained; ++index) {
        const auto& block = profile.slowest[index];
        const auto& events = block.events;
        const auto& voices = block.voices;
        ESP_LOGI(
            kTag,
            "i2s_worst: BLOCK rank=%u stream_block=%u frame=%llu render_us=%u events=%u note_on=%u note_off=%u cc=%u program=%u pitch=%u pressure=%u gm_reset=%u event_channels=0x%04x sustain=0x%04x active=%u releasing=%u lowpass=%u vibrato=%u mod_lfo=%u mod_env=%u fx_send=%u realtime_control=%u fast_release_tails=%u steal_tails=%u exclusive_tails=%u voice_channels=0x%04x",
            static_cast<unsigned>(index + 1u),
            static_cast<unsigned>(block.stream_block),
            static_cast<unsigned long long>(block.block_start_frame),
            static_cast<unsigned>(block.render_us),
            static_cast<unsigned>(events.total),
            static_cast<unsigned>(events.note_on),
            static_cast<unsigned>(events.note_off),
            static_cast<unsigned>(events.control_change),
            static_cast<unsigned>(events.program_change),
            static_cast<unsigned>(events.pitch_bend),
            static_cast<unsigned>(events.channel_pressure),
            static_cast<unsigned>(events.gm_reset),
            static_cast<unsigned>(events.event_channel_mask),
            static_cast<unsigned>(events.sustain_channel_mask),
            static_cast<unsigned>(voices.active),
            static_cast<unsigned>(voices.releasing),
            static_cast<unsigned>(voices.lowpass),
            static_cast<unsigned>(voices.vibrato),
            static_cast<unsigned>(voices.mod_lfo),
            static_cast<unsigned>(voices.mod_env),
            static_cast<unsigned>(voices.fx_send),
            static_cast<unsigned>(voices.realtime_control),
            static_cast<unsigned>(voices.fast_release_tails),
            static_cast<unsigned>(voices.steal_tails),
            static_cast<unsigned>(voices.exclusive_tails),
            static_cast<unsigned>(voices.channel_mask));
    }
}

}  // namespace gm_synth::bringup
