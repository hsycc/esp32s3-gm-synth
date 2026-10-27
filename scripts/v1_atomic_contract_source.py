"""Exact, reversible placement-contract delta; no protocol edits allowed."""
PATCHES = {'firmware/app/LoopProgramRuntime.h': [('#include "voice/experimental/PreparedLoopHandoff.h"', '#include "voice/experimental/PreparedLoopHandoff.h"\n#include "util/RealtimeAtomicContract.h"'), ('    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);\n', ''), ('provider_context_(context), handoff_(directory), coordinator_(samples, playback) {}', 'provider_context_(context), handoff_(directory), coordinator_(samples, playback) {\n        util::requireRealtimeAtomic32(phase_);\n    }')], 'firmware/voice/experimental/PreparedLoopHandoff.h': [('#include <atomic>', '#include <atomic>\n#include "util/RealtimeAtomicContract.h"'), ('    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,\n                  "handoff state requires lock-free 32-bit atomics");\n', ''), ('explicit PreparedLoopHandoff(sample::PublishedSampleDirectory& directory) : directory_(directory) {}', 'explicit PreparedLoopHandoff(sample::PublishedSampleDirectory& directory) : directory_(directory) {\n        util::requireRealtimeAtomic32(phase_);\n    }')]}

def restore_atomic_contract(path, text):
    edits = PATCHES.get(path)
    if not edits or 'util/RealtimeAtomicContract.h' not in text:
        return text
    # Restore deleted assertions at their original position, not by broad regex.
    if path.endswith('LoopProgramRuntime.h'):
        anchor = '    enum class Phase : std::uint32_t { Idle, Preparing, Ready, Committed, Fault };\n'
    else:
        anchor = '    static_assert(MaxPlans > 0 && MaxPlans <= gm::PlaybackProgramSnapshot::kMaxZones);\n'
    if text.count(anchor) != 1:
        raise ValueError('atomic contract anchor changed: ' + path)
    for before, after in reversed(edits):
        if not after:
            text = text.replace(anchor, anchor + before, 1)
        else:
            if text.count(after) != 1:
                raise ValueError('unexpected atomic contract delta: ' + path)
            text = text.replace(after, before, 1)
    return text
