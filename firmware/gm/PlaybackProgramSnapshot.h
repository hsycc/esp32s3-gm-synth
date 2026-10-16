#pragma once

#include "sample/ProgramSnapshot.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

// Immutable, control-domain-prepared playback metadata for one SF2 zone.
// Expensive SF2 generator/modulator merging belongs before publication. The
// audio domain only performs bounded key/velocity matching and copies these
// already-resolved values into VoiceStartParams.
struct PlaybackZone {
    sample::SampleId sample_id = 0;
    std::uint8_t key_low = 0;
    std::uint8_t key_high = 127;
    std::uint8_t velocity_low = 0;
    std::uint8_t velocity_high = 127;
    std::uint16_t exclusive_class = 0;

    // root_phase_increment_q32 already includes source/output sample-rate,
    // coarse/fine tuning and sample pitch-correction terms for root_key.
    // Runtime NoteOn performs equal-temperament semitone transposition with
    // integer fixed-point math only. key_tracking=false is useful for
    // fixed-pitch drum zones.
    std::uint8_t root_key = 60;
    bool key_tracking = true;
    std::uint64_t root_phase_increment_q32 = voice::kPhaseOneQ32;

    // SoundFont address generators are resolved relative to the resident shdr
    // sample. Disabled means use the entire sample-global SampleDescriptor.
    voice::SampleRegion sample_region{};
    voice::LoopMode loop_mode = voice::LoopMode::None;
    std::int32_t gain_left_q15 = voice::kQ15Unity;
    std::int32_t gain_right_q15 = voice::kQ15Unity;
    voice::EnvelopeConfig envelope{};

    bool valid() const {
        return key_low <= key_high && key_high <= 127 &&
               velocity_low <= velocity_high && velocity_high <= 127 &&
               root_key <= 127 && root_phase_increment_q32 != 0 &&
               (!sample_region.enabled ||
                sample_region.start_frame < sample_region.end_frame);
    }

    bool matches(std::uint8_t key, std::uint8_t velocity) const {
        return key >= key_low && key <= key_high &&
               velocity >= velocity_low && velocity <= velocity_high;
    }
};

struct PlaybackProgramSnapshot {
    // This is a realtime scan bound, not an inline-storage size. The checked-in
    // SF2 analysis manifests are validated against it in CI. GeneralUser's
    // largest currently resolved program has 200 zones, so 256 leaves bounded
    // headroom without inflating every per-channel snapshot object.
    static constexpr std::size_t kMaxZones = 256;

    std::uint8_t channel = 0;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint32_t token = 0;
    std::uint32_t bank_generation = 0;

    // Caller-owned immutable storage. The pointed-to zone array must remain
    // alive and unchanged from READY publication until the snapshot is retired
    // and acknowledged by the control domain. This mirrors ProgramSnapshot's
    // caller-owned lifetime while keeping the exchange itself pointer-sized.
    const PlaybackZone* zones = nullptr;
    std::uint16_t zone_count = 0;

    bool valid() const {
        if (channel >= 16 || token == 0 || bank_generation == 0 ||
            zone_count > kMaxZones || (zone_count != 0 && zones == nullptr)) {
            return false;
        }
        for (std::size_t i = 0; i < zone_count; ++i) {
            if (!zones[i].valid()) {
                return false;
            }
        }
        return true;
    }
};

struct PlaybackProgramCommitAck {
    std::uint32_t token = 0;
    const PlaybackProgramSnapshot* retired = nullptr;

    bool valid() const { return token != 0; }
};

struct PlaybackProgramExchangeStats {
    std::uint64_t ready_publishes = 0;
    std::uint64_t ready_rejections = 0;
    std::uint64_t commits = 0;
    std::uint64_t commit_acks = 0;
};

// Playback counterpart to sample::ProgramSnapshotExchange. It intentionally
// contains pointers only; zone arrays and snapshots are caller-owned immutable
// objects prepared outside the realtime domain.
class PlaybackProgramSnapshotExchange {
public:
    static constexpr std::size_t kMidiChannels = 16;

    PlaybackProgramSnapshotExchange();

    bool setInitialActive(const PlaybackProgramSnapshot* snapshot);
    bool publishReady(const PlaybackProgramSnapshot* snapshot);
    bool commitAtBlockBoundary(std::uint8_t channel);
    PlaybackProgramCommitAck acknowledgeCommit(std::uint8_t channel, std::uint32_t token);

    const PlaybackProgramSnapshot* active(std::uint8_t channel) const;
    const PlaybackProgramSnapshot* ready(std::uint8_t channel) const;
    bool transitionInFlight(std::uint8_t channel) const;
    std::uint32_t committedToken(std::uint8_t channel) const;
    PlaybackProgramExchangeStats stats() const;

private:
    struct ChannelSlot {
        ChannelSlot() noexcept
            : active(nullptr), ready(nullptr), retired(nullptr), committed_token(0), in_flight(false) {}

        std::atomic<const PlaybackProgramSnapshot*> active;
        std::atomic<const PlaybackProgramSnapshot*> ready;
        std::atomic<const PlaybackProgramSnapshot*> retired;
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

// Audio-boundary bridge while residency and playback metadata remain separate
// ownership concepts. Control must publish matching READY snapshots to both
// exchanges. Under the existing single-control-owner contract, the preflight
// makes the two pointer commits a non-interleaved block-boundary transaction.
class ProgramRuntimeCommitCoordinator {
public:
    ProgramRuntimeCommitCoordinator(
        sample::ProgramSnapshotExchange& residency,
        PlaybackProgramSnapshotExchange& playback)
        : residency_(residency), playback_(playback) {}

    bool readyCompatible(std::uint8_t channel) const;
    bool activeCompatible(std::uint8_t channel) const;
    bool commitAtBlockBoundary(std::uint8_t channel);

private:
    static bool sameIdentity(
        const sample::ProgramSnapshot& residency,
        const PlaybackProgramSnapshot& playback);

    sample::ProgramSnapshotExchange& residency_;
    PlaybackProgramSnapshotExchange& playback_;
};

}  // namespace gm_synth::gm
