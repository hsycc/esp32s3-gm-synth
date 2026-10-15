#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

struct ProgramChangeRequest {
    std::uint8_t channel = 0;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint32_t token = 0;

    bool valid() const { return channel < 16 && token != 0; }
};

struct ProgramChangeMailboxStats {
    std::uint64_t submitted = 0;
    std::uint64_t superseded = 0;
    std::uint64_t taken = 0;
    std::uint64_t rejected = 0;
};

// Fixed 16-channel latest-wins control mailbox.
//
// It intentionally stores only the compact Program identity, not a copied
// SampleId dependency set. When a request is later serviced, the control
// domain resolves dependencies from the current SF2/ResolvedZone index. This
// avoids adding another ~32-64 KiB of fixed dependency storage merely to queue
// rapid Program Change traffic.
//
// Threading: Phase-B implementation is control-domain only. If MIDI decoding
// later produces requests on another core, it feeds the bounded realtime event
// queue first; the control owner remains the single mailbox mutator.
class ProgramChangeMailbox {
public:
    static constexpr std::size_t kMidiChannels = 16;

    bool submit(const ProgramChangeRequest& request);
    bool take(std::uint8_t channel, ProgramChangeRequest* request);
    bool peek(std::uint8_t channel, ProgramChangeRequest* request) const;
    bool hasPending(std::uint8_t channel) const;
    bool clear(std::uint8_t channel);

    ProgramChangeMailboxStats stats() const { return stats_; }

private:
    struct Slot {
        bool pending = false;
        ProgramChangeRequest request{};
    };

    std::array<Slot, kMidiChannels> slots_{};
    ProgramChangeMailboxStats stats_{};
};

}  // namespace gm_synth::gm
