#include "PitchMath.h"

#include <cmath>
#include <limits>

namespace gm_synth::voice {

std::uint64_t phaseIncrementQ32(
    std::uint32_t source_sample_rate,
    std::uint32_t output_sample_rate,
    double cents_from_sample_root) {
    if (source_sample_rate == 0 || output_sample_rate == 0 ||
        !std::isfinite(cents_from_sample_root)) {
        return 0;
    }

    const double rate_ratio = static_cast<double>(source_sample_rate) /
        static_cast<double>(output_sample_rate);
    const double pitch_ratio = std::exp2(cents_from_sample_root / 1200.0);
    const double increment = rate_ratio * pitch_ratio * 4294967296.0;

    if (!std::isfinite(increment) || increment < 1.0 ||
        increment > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return 0;
    }

    return static_cast<std::uint64_t>(std::llround(increment));
}

}  // namespace gm_synth::voice
