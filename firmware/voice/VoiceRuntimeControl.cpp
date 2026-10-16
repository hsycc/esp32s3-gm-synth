#include "VoiceRenderer.h"

#include "util/SaturatingCounter.h"

namespace gm_synth::voice {

bool Voice::applyRuntimeControlUpdate(const VoiceRuntimeControlUpdate& update) {
    if (!active_ || !update.valid()) {
        return false;
    }
    params_.phase_increment_q32 = update.phase_increment_q32;
    params_.gain_left_q15 = clampQ15(update.gain_left_q15);
    params_.gain_right_q15 = clampQ15(update.gain_right_q15);
    params_.reverb_send_units = update.reverb_send_units;
    params_.chorus_send_units = update.chorus_send_units;
    return true;
}

std::size_t VoicePool::noteOff(
    std::uint8_t channel,
    std::uint8_t key,
    bool sustain_down) {
    std::size_t matches = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        Voice& voice = voices_[index];
        if (!voice.active() || voice.channel() != channel || voice.key() != key) {
            continue;
        }
        ++matches;
        if (sustain_down && !voice.releasing() && !voice.fastReleaseTail()) {
            if (sustain_pending_serials_[index] != start_serials_[index]) {
                sustain_pending_serials_[index] = start_serials_[index];
                util::saturatingIncrement(stats_.sustain_deferred_note_offs);
            }
        } else {
            sustain_pending_serials_[index] = 0;
            voice.noteOff();
        }
    }
    util::saturatingAdd(stats_.note_off_matches, matches);
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
    return matches;
}

std::size_t VoicePool::releaseSustain(std::uint8_t channel) {
    std::size_t released = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        Voice& voice = voices_[index];
        if (!voice.active() || voice.channel() != channel ||
            sustain_pending_serials_[index] == 0 ||
            sustain_pending_serials_[index] != start_serials_[index]) {
            continue;
        }
        sustain_pending_serials_[index] = 0;
        voice.noteOff();
        ++released;
    }
    util::saturatingAdd(stats_.sustain_release_voices, released);
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
    return released;
}

std::size_t VoicePool::allNotesOff(std::uint8_t channel, bool sustain_down) {
    std::size_t matches = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        Voice& voice = voices_[index];
        if (!voice.active() || voice.channel() != channel) {
            continue;
        }
        ++matches;
        if (sustain_down && !voice.releasing() && !voice.fastReleaseTail()) {
            if (sustain_pending_serials_[index] != start_serials_[index]) {
                sustain_pending_serials_[index] = start_serials_[index];
                util::saturatingIncrement(stats_.sustain_deferred_note_offs);
            }
        } else {
            sustain_pending_serials_[index] = 0;
            voice.noteOff();
        }
    }
    util::saturatingAdd(stats_.all_notes_off_matches, matches);
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
    return matches;
}

std::size_t VoicePool::allSoundOff(std::uint8_t channel) {
    std::size_t stopped = 0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        Voice& voice = voices_[index];
        if (!voice.active() || voice.channel() != channel) {
            continue;
        }
        sustain_pending_serials_[index] = 0;
        voice.reset();
        util::saturatingIncrement(stats_.voices_finished);
        ++stopped;
    }
    util::saturatingAdd(stats_.channel_sound_off_voices, stopped);
    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
    return stopped;
}

std::size_t VoicePool::updateActiveVoices(
    std::uint8_t channel,
    ActiveVoiceUpdater updater,
    void* context) {
    if (updater == nullptr) {
        return 0;
    }

    std::size_t updated = 0;
    for (Voice& voice : voices_) {
        if (!voice.active() || voice.channel() != channel) {
            continue;
        }
        if (updater(context, voice)) {
            ++updated;
        }
    }
    util::saturatingAdd(stats_.runtime_control_updates, updated);
    return updated;
}

}  // namespace gm_synth::voice
