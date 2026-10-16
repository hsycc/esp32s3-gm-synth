#pragma once

#include <limits>
#include <type_traits>

namespace gm_synth::util {

template <typename T>
constexpr void saturatingIncrement(T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                  "saturating counters require an unsigned integral type");
    if (value != std::numeric_limits<T>::max()) {
        ++value;
    }
}

template <typename T, typename U>
constexpr void saturatingAdd(T& value, U increment) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                  "saturating counters require an unsigned integral type");
    static_assert(std::is_integral_v<U> && std::is_unsigned_v<U>,
                  "saturating increments require an unsigned integral type");

    using Common = std::common_type_t<T, U>;
    const Common current = static_cast<Common>(value);
    const Common maximum = static_cast<Common>(std::numeric_limits<T>::max());
    const Common delta = static_cast<Common>(increment);
    value = static_cast<T>(delta > maximum - current ? maximum : current + delta);
}

}  // namespace gm_synth::util
