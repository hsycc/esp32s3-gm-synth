#pragma once

#include "SampleResidencyManager.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

struct ProgramSnapshot {
    static constexpr std::size_t kMaxDependencies = SampleResidencyManager::kMaxResidentSamples;

    std::uint8_t channel = 0;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint32_t token = 0;
    std::uint32_t bank_generation = 0;
    std::uint16_t dependency_count = 0;
    std::array<SampleId, kMaxDependencies> sample_ids{};

    bool valid() const {
        return channel < 16 && token != 0 && bank_generation != 0 &&
               dependency_count <= sample_ids.size();
    }
};

struct ProgramCommitAck {
    std::uint32_t token = 0;
    const ProgramSnapshot* retired = nullptr;

    bool valid() const { return token != 0; }
};

struct ProgramExchangeStats {
    std::uint64_t ready_publishes = 0;
    std::uint64_t ready_rejections = 0;
    std::uint64_t commits = 0;
    std::uint64_t commit_acks = 0;
};

// Fixed 16-channel control/audio handoff.
//
// Snapshot memory is caller-owned and immutable while published as ready,
// active, or retired. Control prepares and pins dependencies first, then calls
// publishReady(). Audio calls commitAtBlockBoundary() only at a render-block
// boundary. Control later calls acknowledgeCommit() before retiring old pins.
//
// V1 intentionally allows only one in-flight transition per channel. This
// bounds rapid Program Change state and prevents an unbounded retiring chain.
class ProgramSnapshotExchange {
public:
    static constexpr std::size_t kMidiChannels = 16;

    ProgramSnapshotExchange();

    // Initialization path before realtime playback starts.
    bool setInitialActive(const ProgramSnapshot* snapshot);

    // Control-side READY publication. Fails if a transition for this channel
    // is already awaiting commit/acknowledgement.
    bool publishReady(const ProgramSnapshot* snapshot);

    // Audio-side, block-boundary-only operation. Performs atomic pointer
    // exchange only; no pin/unpin, allocation, storage or dependency scans.
    bool commitAtBlockBoundary(std::uint8_t channel);

    // Control-side acknowledgement. Returns the exact previous Active snapshot
    // that may now have its program pins retired. A nullptr retired snapshot is
    // valid for the first transition from no active program.
    ProgramCommitAck acknowledgeCommit(std::uint8_t channel, std::uint32_t token);

    const ProgramSnapshot* active(std::uint8_t channel) const;
    const ProgramSnapshot* ready(std::uint8_t channel) const;
    bool transitionInFlight(std::uint8_t channel) const;
    std::uint32_t committedToken(std::uint8_t channel) const;

    ProgramExchangeStats stats() const;

private:
    struct ChannelSlot {
        ChannelSlot() noexcept
            : active(nullptr), ready(nullptr), retired(nullptr), committed_token(0), in_flight(false) {}

        std::atomic<const ProgramSnapshot*> active;
        std::atomic<const ProgramSnapshot*> ready;
        std::atomic<const ProgramSnapshot*> retired;
        std::atomic<std::uint32_t> committed_token;
        std::atomic<bool> in_flight;
    };

    bool validChannel(std::uint8_t channel) const { return channel < slots_.size(); }

    std::array<ChannelSlot, kMidiChannels> slots_{};
    std::atomic<std::uint64_t> ready_publishes_{0};
    std::atomic<std::uint64_t> ready_rejections_{0};
    std::atomic<std::uint64_t> commits_{0};
    std::atomic<std::uint64_t> commit_acks_{0};
};

}  // namespace gm_synth::sample
