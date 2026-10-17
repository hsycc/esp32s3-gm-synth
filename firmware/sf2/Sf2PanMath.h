#pragma once

#include "../voice/VoiceRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

// SF2 pan generator range is -500..+500 tenths of a percent. The specified
// mono pan law is equal-power: right=sin((pan+500)*pi/2000), with the left
// channel mirrored. A compact 129-point Q15 quarter-sine table plus bounded
// linear interpolation keeps NoteOn and live CC10 refresh free of libm calls.
constexpr std::array<std::uint16_t, 129> kSf2PanSineQ15{{
    0, 402, 804, 1206, 1608, 2009, 2410, 2811, 3212, 3612, 4011, 4410, 4808, 5205, 5602, 5998,
    6393, 6786, 7179, 7571, 7962, 8351, 8739, 9126, 9512, 9896, 10278, 10659, 11039, 11417, 11793, 12167,
    12539, 12910, 13279, 13645, 14010, 14372, 14732, 15090, 15446, 15800, 16151, 16499, 16846, 17189, 17530, 17869,
    18204, 18537, 18868, 19195, 19519, 19841, 20159, 20475, 20787, 21096, 21403, 21705, 22005, 22301, 22594, 22884,
    23170, 23452, 23731, 24007, 24279, 24547, 24811, 25072, 25329, 25582, 25832, 26077, 26319, 26556, 26790, 27019,
    27245, 27466, 27683, 27896, 28105, 28310, 28510, 28706, 28898, 29085, 29268, 29447, 29621, 29791, 29956, 30117,
    30273, 30424, 30571, 30714, 30852, 30985, 31113, 31237, 31356, 31470, 31580, 31685, 31785, 31880, 31971, 32057,
    32137, 32213, 32285, 32351, 32412, 32469, 32521, 32567, 32609, 32646, 32678, 32705, 32728, 32745, 32757, 32765,
    32767,
}};

inline std::int32_t sf2PanChannelGainQ15(std::int32_t pan_sf2_units, bool left) {
    const std::int32_t pan = std::clamp<std::int32_t>(pan_sf2_units, -500, 500);
    const std::int32_t position = left ? 500 - pan : pan + 500;
    if (position <= 0) {
        return 0;
    }
    if (position >= 1000) {
        return voice::kQ15Unity;
    }

    constexpr std::uint64_t kSegments = kSf2PanSineQ15.size() - 1u;
    const std::uint64_t position_q16 =
        (static_cast<std::uint64_t>(position) * kSegments * 65536u + 500u) / 1000u;
    const std::size_t lower = std::min<std::size_t>(
        static_cast<std::size_t>(position_q16 >> 16u),
        kSf2PanSineQ15.size() - 1u);
    if (lower + 1u >= kSf2PanSineQ15.size()) {
        return kSf2PanSineQ15.back();
    }

    const std::uint32_t fraction_q16 = static_cast<std::uint32_t>(position_q16 & 0xffffu);
    const std::int32_t a = kSf2PanSineQ15[lower];
    const std::int32_t b = kSf2PanSineQ15[lower + 1u];
    const std::int64_t delta = static_cast<std::int64_t>(b) - a;
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(a) +
        (delta * fraction_q16 + 32768) / 65536);
}

}  // namespace gm_synth::sf2
