#include "ProgramRuntimeAudioBlock.h"

#include "util/SaturatingCounter.h"

namespace gm_synth::gm {

std::size_t ProgramRuntimeBlockCommitter::commitReadyAtBlockBoundary() {
    util::saturatingIncrement(stats_.blocks);
    std::size_t committed = 0;
    for (std::size_t channel = 0;
         channel < PlaybackProgramSnapshotExchange::kMidiChannels;
         ++channel) {
        const auto midi_channel = static_cast<std::uint8_t>(channel);
        if (!coordinator_.readyCompatible(midi_channel)) {
            continue;
        }

        util::saturatingIncrement(stats_.ready_channels);
        if (coordinator_.commitAtBlockBoundary(midi_channel)) {
            ++committed;
            util::saturatingIncrement(stats_.commits);
        } else {
            util::saturatingIncrement(stats_.commit_failures);
        }
    }
    return committed;
}

}  // namespace gm_synth::gm
