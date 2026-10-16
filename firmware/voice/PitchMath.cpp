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

    const long double rate_ratio = static_cast<long double>(source_sample_rate) /
        static_cast<long double>(output_sample_rate);
    const long double pitch_ratio = std::exp2(cents_from_sample_root / 1200.0);
    const long double increment = rate_ratio * pitch_ratio * 4294967296.0L;
    const long double max_increment =
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max());

    if (!std::isfinite(increment) || increment < 1.0L || increment >= max_increment) {
        return 0;
    }

    return static_cast<std::uint64_t>(increment + 0.5L);
}

}  // namespace gm_synth::voice
