#include "../../firmware/voice/PitchMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::phaseIncrementQ32;

void expectNear(std::uint64_t actual, std::uint64_t expected, std::uint64_t tolerance = 2) {
    const std::uint64_t delta = actual > expected ? actual - expected : expected - actual;
    assert(delta <= tolerance);
}

void testUnityAtMatchingSampleRate() {
    expectNear(phaseIncrementQ32(44100, 44100, 0.0), kPhaseOneQ32);
}

void testOctaves() {
    expectNear(phaseIncrementQ32(44100, 44100, 1200.0), kPhaseOneQ32 * 2u);
    expectNear(phaseIncrementQ32(44100, 44100, -1200.0), kPhaseOneQ32 / 2u);
}

void testSourceRateConversion() {
    expectNear(phaseIncrementQ32(22050, 44100, 0.0), kPhaseOneQ32 / 2u);
    expectNear(phaseIncrementQ32(48000, 48000, 0.0), kPhaseOneQ32);
    expectNear(phaseIncrementQ32(48000, 24000, 0.0), kPhaseOneQ32 * 2u);
}

void testSemitoneRatio() {
    const auto increment = phaseIncrementQ32(44100, 44100, 100.0);
    const auto expected = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(kPhaseOneQ32) * std::exp2(1.0 / 12.0)));
    expectNear(increment, expected);
}

void testInvalidInputsFailClosed() {
    assert(phaseIncrementQ32(0, 44100, 0.0) == 0);
    assert(phaseIncrementQ32(44100, 0, 0.0) == 0);
    assert(phaseIncrementQ32(44100, 44100, std::nan("")) == 0);
    assert(phaseIncrementQ32(44100, 44100, std::numeric_limits<double>::infinity()) == 0);
    assert(phaseIncrementQ32(44100, 44100, 1.0e9) == 0);
    assert(phaseIncrementQ32(44100, 44100, -1.0e9) == 0);
}

}  // namespace

int main() {
    testUnityAtMatchingSampleRate();
    testOctaves();
    testSourceRateConversion();
    testSemitoneRatio();
    testInvalidInputsFailClosed();
    std::cout << "pitch math tests passed\n";
    return 0;
}
