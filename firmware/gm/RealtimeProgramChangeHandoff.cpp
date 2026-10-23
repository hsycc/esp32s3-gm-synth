#include "RealtimeProgramChangeHandoff.h"

namespace gm_synth::gm {
namespace {

void saturatingAtomicIncrement(std::atomic<std::uint32_t>& value) {
    std::uint32_t current = value.load(std::memory_order_relaxed);
    while (current != UINT32_MAX &&
           !value.compare_exchange_weak(
               current,
               current + 1u,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

}  // namespace

std::uint32_t RealtimeProgramChangeHandoff::packIdentity(
    std::uint16_t bank,
    std::uint8_t program) {
    return (static_cast<std::uint32_t>(bank & 0x3fffu)) |
           (static_cast<std::uint32_t>(program & 0x7fu) << 14u);
}

void RealtimeProgramChangeHandoff::unpackIdentity(
    std::uint32_t packed,
    std::uint16_t* bank,
    std::uint8_t* program) {
    if (bank != nullptr) {
        *bank = static_cast<std::uint16_t>(packed & 0x3fffu);
    }
    if (program != nullptr) {
        *program = static_cast<std::uint8_t>((packed >> 14u) & 0x7fu);
    }
}

bool RealtimeProgramChangeHandoff::publish(const ProgramChangeRequest& request) {
    if (!request.valid() || request.bank > 0x3fffu || request.program > 127) {
        saturatingAtomicIncrement(rejected_);
        return false;
    }

    Slot& slot = slots_[request.channel];
    const std::uint32_t previous_token = slot.token.load(std::memory_order_acquire);
    const std::uint32_t consumed = slot.consumed_token.load(std::memory_order_acquire);
    if (previous_token != 0 && previous_token != consumed) {
        saturatingAtomicIncrement(superseded_);
    }

    // Single producer: odd means the payload is being replaced, even means it
    // is stable. All fields are 32-bit atomics so the ESP32-S3 path remains on
    // its native lock-free atomic primitive.
    slot.sequence.fetch_add(1u, std::memory_order_acq_rel);
    slot.packed_identity.store(
        packIdentity(request.bank, request.program),
        std::memory_order_relaxed);
    slot.token.store(request.token, std::memory_order_relaxed);
    slot.sequence.fetch_add(1u, std::memory_order_release);

    saturatingAtomicIncrement(published_);
    return true;
}

bool RealtimeProgramChangeHandoff::takeLatest(
    std::uint8_t channel,
    ProgramChangeRequest* request) {
    if (channel >= slots_.size() || request == nullptr) {
        saturatingAtomicIncrement(rejected_);
        return false;
    }

    Slot& slot = slots_[channel];
    for (;;) {
        const std::uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            saturatingAtomicIncrement(read_retries_);
            continue;
        }

        const std::uint32_t packed = slot.packed_identity.load(std::memory_order_relaxed);
        const std::uint32_t token = slot.token.load(std::memory_order_relaxed);
        const std::uint32_t after = slot.sequence.load(std::memory_order_acquire);
        if (before != after || (after & 1u) != 0u) {
            saturatingAtomicIncrement(read_retries_);
            continue;
        }

        if (token == 0 || token == last_taken_token_[channel]) {
            return false;
        }

        ProgramChangeRequest candidate{};
        candidate.channel = channel;
        unpackIdentity(packed, &candidate.bank, &candidate.program);
        candidate.token = token;
        if (!candidate.valid()) {
            saturatingAtomicIncrement(rejected_);
            return false;
        }

        *request = candidate;
        last_taken_token_[channel] = token;
        slot.consumed_token.store(token, std::memory_order_release);
        saturatingAtomicIncrement(taken_);
        return true;
    }
}

std::size_t RealtimeProgramChangeHandoff::drainToMailbox(
    ProgramChangeMailbox& mailbox) {
    std::size_t submitted = 0;
    for (std::size_t channel = 0; channel < slots_.size(); ++channel) {
        ProgramChangeRequest request{};
        if (!takeLatest(static_cast<std::uint8_t>(channel), &request)) {
            continue;
        }
        if (mailbox.submit(request)) {
            ++submitted;
        }
    }
    return submitted;
}

RealtimeProgramChangeHandoffStats RealtimeProgramChangeHandoff::stats() const {
    RealtimeProgramChangeHandoffStats out{};
    out.published = published_.load(std::memory_order_relaxed);
    out.superseded = superseded_.load(std::memory_order_relaxed);
    out.taken = taken_.load(std::memory_order_relaxed);
    out.rejected = rejected_.load(std::memory_order_relaxed);
    out.read_retries = read_retries_.load(std::memory_order_relaxed);
    return out;
}

}  // namespace gm_synth::gm
