#include "SampleResidencyManager.h"

namespace gm_synth::sample {

ResidentSampleView SampleResidencyManager::residentView(SampleId sample_id) const {
    ResidentSampleView view{};
    if (!initialized_) {
        return view;
    }

    const int index = findEntryIndex(sample_id);
    if (index < 0) {
        return view;
    }

    const Entry& entry = entries_[static_cast<std::size_t>(index)];
    if (!entry.occupied) {
        return view;
    }

    view.sample_id = sample_id;
    view.generation = entry.generation;
    view.pcm = reinterpret_cast<const std::int16_t*>(arena_ + entry.offset);
    view.descriptor = entry.descriptor;
    view.pin_count = entry.pin_count;
    view.phase_a_voice_ref_count = entry.voice_ref_count;
    return view;
}

}  // namespace gm_synth::sample
