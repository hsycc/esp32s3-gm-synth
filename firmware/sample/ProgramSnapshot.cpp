#include "ProgramSnapshot.h"

namespace gm_synth::sample {

ProgramSnapshotExchange::ProgramSnapshotExchange() = default;

bool ProgramSnapshotExchange::setInitialActive(const ProgramSnapshot* snapshot) {
    if (snapshot == nullptr || !snapshot->valid() || !validChannel(snapshot->channel)) {
        return false;
    }

    ChannelSlot& slot = slots_[snapshot->channel];
    if (slot.in_flight.load(std::memory_order_acquire) ||
        slot.ready.load(std::memory_order_acquire) != nullptr ||
        slot.retired.load(std::memory_order_acquire) != nullptr) {
        return false;
    }

    const ProgramSnapshot* expected = nullptr;
    return slot.active.compare_exchange_strong(
        expected,
        snapshot,
        std::memory_order_release,
        std::memory_order_acquire);
}

bool ProgramSnapshotExchange::publishReady(const ProgramSnapshot* snapshot) {
    if (snapshot == nullptr || !snapshot->valid() || !validChannel(snapshot->channel)) {
        ready_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ChannelSlot& slot = slots_[snapshot->channel];
    bool expected_in_flight = false;
    if (!slot.in_flight.compare_exchange_strong(
            expected_in_flight,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        ready_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (slot.ready.load(std::memory_order_acquire) != nullptr ||
        slot.retired.load(std::memory_order_acquire) != nullptr ||
        slot.committed_token.load(std::memory_order_acquire) != 0) {
        slot.in_flight.store(false, std::memory_order_release);
        ready_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    slot.ready.store(snapshot, std::memory_order_release);
    ready_publishes_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ProgramSnapshotExchange::commitAtBlockBoundary(std::uint8_t channel) {
    if (!validChannel(channel)) {
        return false;
    }

    ChannelSlot& slot = slots_[channel];
    if (!slot.in_flight.load(std::memory_order_acquire)) {
        return false;
    }

    const ProgramSnapshot* next = slot.ready.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) {
        return false;
    }

    const ProgramSnapshot* previous = slot.active.exchange(next, std::memory_order_acq_rel);

    // With one in-flight transition per channel there can be only one retired
    // snapshot awaiting control acknowledgement.
    const ProgramSnapshot* expected_retired = nullptr;
    if (!slot.retired.compare_exchange_strong(
            expected_retired,
            previous,
            std::memory_order_release,
            std::memory_order_acquire)) {
        // This is an invariant violation. Restore the previous active/ready
        // state so audio never exposes an unacknowledgeable transition.
        slot.active.store(previous, std::memory_order_release);
        slot.ready.store(next, std::memory_order_release);
        return false;
    }

    slot.committed_token.store(next->token, std::memory_order_release);
    commits_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ProgramCommitAck ProgramSnapshotExchange::acknowledgeCommit(
    std::uint8_t channel,
    std::uint32_t token) {
    ProgramCommitAck ack{};
    if (!validChannel(channel) || token == 0) {
        return ack;
    }

    ChannelSlot& slot = slots_[channel];
    if (!slot.in_flight.load(std::memory_order_acquire) ||
        slot.committed_token.load(std::memory_order_acquire) != token ||
        slot.ready.load(std::memory_order_acquire) != nullptr) {
        return ack;
    }

    ack.token = token;
    ack.retired = slot.retired.exchange(nullptr, std::memory_order_acq_rel);
    slot.committed_token.store(0, std::memory_order_release);
    slot.in_flight.store(false, std::memory_order_release);
    commit_acks_.fetch_add(1, std::memory_order_relaxed);
    return ack;
}

const ProgramSnapshot* ProgramSnapshotExchange::active(std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return nullptr;
    }
    return slots_[channel].active.load(std::memory_order_acquire);
}

const ProgramSnapshot* ProgramSnapshotExchange::ready(std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return nullptr;
    }
    return slots_[channel].ready.load(std::memory_order_acquire);
}

bool ProgramSnapshotExchange::transitionInFlight(std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return false;
    }
    return slots_[channel].in_flight.load(std::memory_order_acquire);
}

std::uint32_t ProgramSnapshotExchange::committedToken(std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return 0;
    }
    return slots_[channel].committed_token.load(std::memory_order_acquire);
}

ProgramExchangeStats ProgramSnapshotExchange::stats() const {
    ProgramExchangeStats out{};
    out.ready_publishes = ready_publishes_.load(std::memory_order_relaxed);
    out.ready_rejections = ready_rejections_.load(std::memory_order_relaxed);
    out.commits = commits_.load(std::memory_order_relaxed);
    out.commit_acks = commit_acks_.load(std::memory_order_relaxed);
    return out;
}

}  // namespace gm_synth::sample
