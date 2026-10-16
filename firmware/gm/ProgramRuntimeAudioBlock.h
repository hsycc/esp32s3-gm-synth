#pragma once

#include "PlaybackProgramSnapshot.h"
#include "realtime/ScheduledAudioBlockRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

struct ProgramRuntimeBlockStats {
    std::uint64_t blocks = 0;
    std::uint64_t ready_channels = 0;
    std::uint64_t commits = 0;
    std::uint64_t commit_failures = 0;
};

// Audio-side block-boundary adapter. It scans the fixed 16 MIDI channels and
// atomically commits only residency/playback READY pairs whose identity matches.
// No parsing, dependency scans, pinning, allocation or storage access occurs.
class ProgramRuntimeBlockCommitter {
public:
    explicit ProgramRuntimeBlockCommitter(ProgramRuntimeCommitCoordinator& coordinator)
        : coordinator_(coordinator) {}

    std::size_t commitReadyAtBlockBoundary();
    const ProgramRuntimeBlockStats& stats() const { return stats_; }

private:
    ProgramRuntimeCommitCoordinator& coordinator_;
    ProgramRuntimeBlockStats stats_{};
};

// Convenience production-shaped wrapper: commit matching Program READY pairs at
// the start of each audio block, then execute the existing sample-accurate event
// renderer unchanged.
class ProgramAwareScheduledAudioBlockRenderer {
public:
    ProgramAwareScheduledAudioBlockRenderer(
        ProgramRuntimeBlockCommitter& committer,
        realtime::ScheduledAudioBlockRenderer& renderer)
        : committer_(committer), renderer_(renderer) {}

    void render(std::uint64_t block_start_frame, std::int16_t* interleaved_stereo) {
        (void)committer_.commitReadyAtBlockBoundary();
        renderer_.render(block_start_frame, interleaved_stereo);
    }

private:
    ProgramRuntimeBlockCommitter& committer_;
    realtime::ScheduledAudioBlockRenderer& renderer_;
};

}  // namespace gm_synth::gm
