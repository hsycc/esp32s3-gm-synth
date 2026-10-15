#include "ProgramChangeMailbox.h"

namespace gm_synth::gm {

bool ProgramChangeMailbox::submit(const ProgramChangeRequest& request) {
    if (!request.valid()) {
        ++stats_.rejected;
        return false;
    }

    Slot& slot = slots_[request.channel];
    if (slot.pending) {
        ++stats_.superseded;
    }
    slot.request = request;
    slot.pending = true;
    ++stats_.submitted;
    return true;
}

bool ProgramChangeMailbox::take(
    std::uint8_t channel,
    ProgramChangeRequest* request) {
    if (channel >= slots_.size() || request == nullptr) {
        ++stats_.rejected;
        return false;
    }

    Slot& slot = slots_[channel];
    if (!slot.pending) {
        return false;
    }

    *request = slot.request;
    slot.request = ProgramChangeRequest{};
    slot.pending = false;
    ++stats_.taken;
    return true;
}

bool ProgramChangeMailbox::peek(
    std::uint8_t channel,
    ProgramChangeRequest* request) const {
    if (channel >= slots_.size() || request == nullptr || !slots_[channel].pending) {
        return false;
    }
    *request = slots_[channel].request;
    return true;
}

bool ProgramChangeMailbox::hasPending(std::uint8_t channel) const {
    return channel < slots_.size() && slots_[channel].pending;
}

bool ProgramChangeMailbox::clear(std::uint8_t channel) {
    if (channel >= slots_.size()) {
        ++stats_.rejected;
        return false;
    }
    Slot& slot = slots_[channel];
    if (!slot.pending) {
        return false;
    }
    slot.request = ProgramChangeRequest{};
    slot.pending = false;
    return true;
}

}  // namespace gm_synth::gm
