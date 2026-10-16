#include "RealtimeProgramChangeRouter.h"

#include "util/SaturatingCounter.h"

namespace gm_synth::gm {

std::uint32_t RealtimeProgramChangeRouter::allocateProgramToken() {
    const std::uint32_t token = next_program_token_;
    ++next_program_token_;
    if (next_program_token_ == 0) {
        next_program_token_ = 1;
    }
    return token;
}

bool RealtimeProgramChangeRouter::apply(
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
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

    ProgramChangeRequest request{};
    request.channel = event.channel;
    request.bank = state.selectedBank();
    request.program = event.data1;
    request.token = allocateProgramToken();
    if (!handoff_.publish(request)) {
        util::saturatingIncrement(stats_.requests_rejected);
        return false;
    }

    util::saturatingIncrement(stats_.requests_published);
    return true;
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
