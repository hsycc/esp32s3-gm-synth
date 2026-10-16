#include "PlaybackProgramSnapshot.h"

namespace gm_synth::gm {

PlaybackProgramSnapshotExchange::PlaybackProgramSnapshotExchange() = default;

bool PlaybackProgramSnapshotExchange::setInitialActive(
    const PlaybackProgramSnapshot* snapshot) {
    if (snapshot == nullptr || !snapshot->valid() || !validChannel(snapshot->channel)) {
        return false;
    }

    ChannelSlot& slot = slots_[snapshot->channel];
    if (slot.in_flight.load(std::memory_order_acquire) ||
        slot.ready.load(std::memory_order_acquire) != nullptr ||
        slot.retired.load(std::memory_order_acquire) != nullptr) {
        return false;
    }

    const PlaybackProgramSnapshot* expected = nullptr;
    return slot.active.compare_exchange_strong(
        expected,
        snapshot,
        std::memory_order_release,
        std::memory_order_acquire);
}

bool PlaybackProgramSnapshotExchange::publishReady(
    const PlaybackProgramSnapshot* snapshot) {
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

bool PlaybackProgramSnapshotExchange::commitAtBlockBoundary(std::uint8_t channel) {
    if (!validChannel(channel)) {
        return false;
    }

    ChannelSlot& slot = slots_[channel];
    if (!slot.in_flight.load(std::memory_order_acquire)) {
        return false;
    }

    const PlaybackProgramSnapshot* next =
        slot.ready.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) {
        return false;
    }

    const PlaybackProgramSnapshot* previous =
        slot.active.exchange(next, std::memory_order_acq_rel);

    const PlaybackProgramSnapshot* expected_retired = nullptr;
    if (!slot.retired.compare_exchange_strong(
            expected_retired,
            previous,
            std::memory_order_release,
            std::memory_order_acquire)) {
        slot.active.store(previous, std::memory_order_release);
        slot.ready.store(next, std::memory_order_release);
        return false;
    }

    slot.committed_token.store(next->token, std::memory_order_release);
    commits_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

PlaybackProgramCommitAck PlaybackProgramSnapshotExchange::acknowledgeCommit(
    std::uint8_t channel,
    std::uint32_t token) {
    PlaybackProgramCommitAck ack{};
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

const PlaybackProgramSnapshot* PlaybackProgramSnapshotExchange::active(
    std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return nullptr;
    }
    return slots_[channel].active.load(std::memory_order_acquire);
}

const PlaybackProgramSnapshot* PlaybackProgramSnapshotExchange::ready(
    std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return nullptr;
    }
    return slots_[channel].ready.load(std::memory_order_acquire);
}

bool PlaybackProgramSnapshotExchange::transitionInFlight(std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return false;
    }
    return slots_[channel].in_flight.load(std::memory_order_acquire);
}

std::uint32_t PlaybackProgramSnapshotExchange::committedToken(
    std::uint8_t channel) const {
    if (!validChannel(channel)) {
        return 0;
    }
    return slots_[channel].committed_token.load(std::memory_order_acquire);
}

PlaybackProgramExchangeStats PlaybackProgramSnapshotExchange::stats() const {
    PlaybackProgramExchangeStats out{};
    out.ready_publishes = ready_publishes_.load(std::memory_order_relaxed);
    out.ready_rejections = ready_rejections_.load(std::memory_order_relaxed);
    out.commits = commits_.load(std::memory_order_relaxed);
    out.commit_acks = commit_acks_.load(std::memory_order_relaxed);
    return out;
}

bool ProgramRuntimeCommitCoordinator::sameIdentity(
    const sample::ProgramSnapshot& residency,
    const PlaybackProgramSnapshot& playback) {
    return residency.channel == playback.channel &&
           residency.bank == playback.bank &&
           residency.program == playback.program &&
           residency.token == playback.token &&
           residency.bank_generation == playback.bank_generation;
}

bool ProgramRuntimeCommitCoordinator::readyCompatible(std::uint8_t channel) const {
    const sample::ProgramSnapshot* residency = residency_.ready(channel);
    const PlaybackProgramSnapshot* playback = playback_.ready(channel);
    return residency != nullptr && playback != nullptr &&
           residency_.transitionInFlight(channel) &&
           playback_.transitionInFlight(channel) &&
           residency_.committedToken(channel) == 0 &&
           playback_.committedToken(channel) == 0 &&
           sameIdentity(*residency, *playback);
}

bool ProgramRuntimeCommitCoordinator::activeCompatible(std::uint8_t channel) const {
    const sample::ProgramSnapshot* residency = residency_.active(channel);
    const PlaybackProgramSnapshot* playback = playback_.active(channel);
    return residency != nullptr && playback != nullptr &&
           sameIdentity(*residency, *playback);
}

bool ProgramRuntimeCommitCoordinator::commitAtBlockBoundary(std::uint8_t channel) {
    if (!readyCompatible(channel)) {
        return false;
    }

    // No control-side mutation is allowed between READY and audio ACK in the
    // one-transition-per-channel protocol. After the compatibility preflight,
    // both commit operations therefore have identical success preconditions.
    if (!residency_.commitAtBlockBoundary(channel)) {
        return false;
    }
    return playback_.commitAtBlockBoundary(channel);
}

}  // namespace gm_synth::gm
