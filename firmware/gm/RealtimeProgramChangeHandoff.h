#pragma once

#include "ProgramChangeMailbox.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "xtensa/config/core-isa.h"
#endif

namespace gm_synth::gm {

struct RealtimeProgramChangeHandoffStats {
    std::uint32_t published = 0;
    std::uint32_t superseded = 0;
    std::uint32_t taken = 0;
    std::uint32_t rejected = 0;
    std::uint32_t read_retries = 0;
};

// Audio -> control latest-wins handoff for Program Change identities.
//
// One audio-side producer publishes compact {bank, program, token} state into
// each channel slot; one control-side consumer drains it later. A 32-bit
// seqlock keeps the two payload words coherent without locks, allocation, or
// storage work on the audio thread. Intermediate changes on the same channel
// are intentionally coalesced.
class RealtimeProgramChangeHandoff {
public:
    static constexpr std::size_t kMidiChannels = 16;

#if defined(ESP_PLATFORM)
    static_assert(XCHAL_HAVE_S32C1I == 1, "Program handoff requires ESP32-S3 32-bit atomics");
#else
    static_assert(
        std::atomic<std::uint32_t>::is_always_lock_free,
        "Program handoff host tests require lock-free 32-bit atomics");
#endif

    bool publish(const ProgramChangeRequest& request);
    bool takeLatest(std::uint8_t channel, ProgramChangeRequest* request);

    // Control-domain convenience bridge into the existing control-only
    // ProgramChangeMailbox. Returns the number of channel requests submitted.
    std::size_t drainToMailbox(ProgramChangeMailbox& mailbox);

    RealtimeProgramChangeHandoffStats stats() const;

private:
    struct Slot {
        std::atomic<std::uint32_t> sequence{0};
        std::atomic<std::uint32_t> packed_identity{0};
        std::atomic<std::uint32_t> token{0};
        std::atomic<std::uint32_t> consumed_token{0};
    };

    static std::uint32_t packIdentity(std::uint16_t bank, std::uint8_t program);
    static void unpackIdentity(
        std::uint32_t packed,
        std::uint16_t* bank,
        std::uint8_t* program);

    std::array<Slot, kMidiChannels> slots_{};
    std::array<std::uint32_t, kMidiChannels> last_taken_token_{};

    std::atomic<std::uint32_t> published_{0};
    std::atomic<std::uint32_t> superseded_{0};
    std::atomic<std::uint32_t> taken_{0};
    std::atomic<std::uint32_t> rejected_{0};
    std::atomic<std::uint32_t> read_retries_{0};
};

}  // namespace gm_synth::gm
