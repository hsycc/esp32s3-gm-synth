#include "../../firmware/util/SaturatingCounter.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

void testIncrementSaturates() {
    std::uint64_t value = std::numeric_limits<std::uint64_t>::max() - 1u;
    gm_synth::util::saturatingIncrement(value);
    assert(value == std::numeric_limits<std::uint64_t>::max());
    gm_synth::util::saturatingIncrement(value);
    assert(value == std::numeric_limits<std::uint64_t>::max());
}

void testAddSaturates() {
    std::uint64_t value = std::numeric_limits<std::uint64_t>::max() - 5u;
    gm_synth::util::saturatingAdd(value, std::uint64_t{4});
    assert(value == std::numeric_limits<std::uint64_t>::max() - 1u);
    gm_synth::util::saturatingAdd(value, std::uint64_t{100});
    assert(value == std::numeric_limits<std::uint64_t>::max());
}

void testSmallUnsignedTypes() {
    std::uint8_t value = 250;
    gm_synth::util::saturatingAdd(value, std::uint8_t{4});
    assert(value == 254);
    gm_synth::util::saturatingAdd(value, std::uint8_t{10});
    assert(value == 255);
    gm_synth::util::saturatingIncrement(value);
    assert(value == 255);
}

}  // namespace

int main() {
    testIncrementSaturates();
    testAddSaturates();
    testSmallUnsignedTypes();
    std::cout << "saturating counter tests passed\n";
    return 0;
}
