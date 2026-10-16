#include "RealtimeProgramChangeRouter.h"

#include "util/SaturatingCounter.h"

namespace gm_synth::gm {
namespace {

constexpr std::uint8_t kMidiChannelCount =
    static_cast<std::uint8_t>(MidiChannelStateBank::kMidiChannels);

}  // namespace

std::uint32_t RealtimeProgramChangeRouter::allocateProgramToken() {
    const std::uint32_t token = next_program_token_;
    ++next_program_token_;
    if (next_program_token_ == 0) {
        next_program_token_ = 1;
    }
    return token;
}

bool RealtimeProgramChangeRouter::publishProgramRequest(
    std::uint8_t channel,
    std::uint8_t program) {
    if (channel >= kMidiChannelCount || program > 127) {
        util::saturatingIncrement(stats_.requests_rejected);
        return false;
    }

    ProgramChangeRequest request{};
    request.channel = channel;
    request.bank = events_.channelStates().programBank(channel);
    request.program = program;
    request.token = allocateProgramToken();
    if (!handoff_.publish(request)) {
        util::saturatingIncrement(stats_.requests_rejected);
        return false;
    }

    util::saturatingIncrement(stats_.requests_published);
    return true;
}

bool RealtimeProgramChangeRouter::applyGmReset(voice::VoicePool& voices) {
    // GM System On is intentionally bounded on the audio side: stop the current
    // voices, reset 16 small channel-state records, and publish at most one
    // compact latest-wins Program identity per channel. All SF2 parsing/sample
    // loading remains in the control task.
    voices.allSoundOff();
    events_.channelStates().resetGm();

    bool success = true;
    for (std::uint8_t channel = 0; channel < kMidiChannelCount; ++channel) {
        if (publishProgramRequest(channel, 0)) {
            util::saturatingIncrement(stats_.gm_reset_program_requests);
        } else {
            success = false;
        }
    }
    return success;
}

bool RealtimeProgramChangeRouter::apply(
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (event.type == realtime::EventType::GmReset) {
        util::saturatingIncrement(stats_.gm_reset_events);
        if (!event.valid()) {
            util::saturatingIncrement(stats_.requests_rejected);
            return false;
        }
        return applyGmReset(voices);
    }

    if (event.type != realtime::EventType::ProgramChange) {
        return events_.apply(event, voices);
    }

    util::saturatingIncrement(stats_.program_change_events);
    if (!event.valid() || event.data1 > 127) {
        util::saturatingIncrement(stats_.requests_rejected);
        return false;
    }

    MidiChannelState& state = events_.channelStates().channel(event.channel);
    if (!state.applyProgramChange(event.data1)) {
        util::saturatingIncrement(stats_.requests_rejected);
        return false;
    }

    return publishProgramRequest(event.channel, event.data1);
}

bool RealtimeProgramChangeRouter::callback(
    void* context,
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (context == nullptr) {
        return false;
    }
    return static_cast<RealtimeProgramChangeRouter*>(context)->apply(event, voices);
}

}  // namespace gm_synth::gm
