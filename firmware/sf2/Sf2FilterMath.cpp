#include "Sf2FilterMath.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {
namespace {

constexpr std::int32_t kTableStepCents = 25;
constexpr std::size_t kTableEntries =
    static_cast<std::size_t>((kSf2MaxFilterCutoffCents - kSf2MinFilterCutoffCents) /
                             kTableStepCents) +
    1u;

// Q16 tan(pi * Fc / 44100), Fc = 8.176 * 2^(cents/1200), sampled every 25 cents.
// The table is generated offline; linear interpolation keeps NoteOn free of
// tan()/pow() while staying below ~1% K error over the supported SF2 range.
constexpr std::array<std::uint32_t, kTableEntries> kTanCutoffQ16{{
    91u, 92u, 93u, 95u, 96u, 98u, 99u, 100u, 102u, 103u, 105u, 106u,
    108u, 110u, 111u, 113u, 114u, 116u, 118u, 119u, 121u, 123u, 125u, 127u,
    128u, 130u, 132u, 134u, 136u, 138u, 140u, 142u, 144u, 146u, 148u, 150u,
    153u, 155u, 157u, 159u, 162u, 164u, 167u, 169u, 171u, 174u, 176u, 179u,
    182u, 184u, 187u, 190u, 192u, 195u, 198u, 201u, 204u, 207u, 210u, 213u,
    216u, 219u, 222u, 225u, 229u, 232u, 235u, 239u, 242u, 246u, 249u, 253u,
    257u, 261u, 264u, 268u, 272u, 276u, 280u, 284u, 288u, 292u, 297u, 301u,
    305u, 310u, 314u, 319u, 324u, 328u, 333u, 338u, 343u, 348u, 353u, 358u,
    363u, 368u, 374u, 379u, 385u, 390u, 396u, 402u, 408u, 414u, 420u, 426u,
    432u, 438u, 445u, 451u, 458u, 464u, 471u, 478u, 485u, 492u, 499u, 506u,
    514u, 521u, 529u, 536u, 544u, 552u, 560u, 568u, 576u, 585u, 593u, 602u,
    611u, 620u, 629u, 638u, 647u, 656u, 666u, 676u, 686u, 696u, 706u, 716u,
    726u, 737u, 748u, 758u, 770u, 781u, 792u, 804u, 815u, 827u, 839u, 851u,
    864u, 876u, 889u, 902u, 915u, 928u, 942u, 956u, 970u, 984u, 998u, 1012u,
    1027u, 1042u, 1057u, 1073u, 1088u, 1104u, 1120u, 1136u, 1153u, 1170u, 1187u, 1204u,
    1222u, 1239u, 1257u, 1276u, 1294u, 1313u, 1332u, 1352u, 1371u, 1391u, 1411u, 1432u,
    1453u, 1474u, 1495u, 1517u, 1539u, 1562u, 1584u, 1607u, 1631u, 1655u, 1679u, 1703u,
    1728u, 1753u, 1778u, 1804u, 1831u, 1857u, 1884u, 1912u, 1940u, 1968u, 1996u, 2025u,
    2055u, 2085u, 2115u, 2146u, 2177u, 2209u, 2241u, 2274u, 2307u, 2340u, 2374u, 2409u,
    2444u, 2480u, 2516u, 2552u, 2590u, 2627u, 2666u, 2704u, 2744u, 2784u, 2824u, 2865u,
    2907u, 2949u, 2992u, 3036u, 3080u, 3125u, 3171u, 3217u, 3264u, 3311u, 3359u, 3408u,
    3458u, 3508u, 3560u, 3611u, 3664u, 3717u, 3772u, 3827u, 3882u, 3939u, 3996u, 4055u,
    4114u, 4174u, 4235u, 4297u, 4359u, 4423u, 4487u, 4553u, 4619u, 4687u, 4755u, 4825u,
    4895u, 4966u, 5039u, 5113u, 5187u, 5263u, 5340u, 5418u, 5497u, 5577u, 5659u, 5742u,
    5826u, 5911u, 5997u, 6085u, 6174u, 6264u, 6356u, 6449u, 6543u, 6639u, 6737u, 6835u,
    6935u, 7037u, 7140u, 7245u, 7351u, 7459u, 7568u, 7680u, 7792u, 7907u, 8023u, 8141u,
    8260u, 8382u, 8505u, 8630u, 8757u, 8886u, 9017u, 9150u, 9285u, 9422u, 9561u, 9702u,
    9845u, 9990u, 10138u, 10288u, 10440u, 10594u, 10751u, 10910u, 11072u, 11236u, 11403u, 11572u,
    11744u, 11919u, 12096u, 12276u, 12459u, 12644u, 12833u, 13024u, 13219u, 13416u, 13617u, 13821u,
    14028u, 14238u, 14452u, 14669u, 14890u, 15114u, 15341u, 15573u, 15808u, 16047u, 16290u, 16537u,
    16787u, 17042u, 17302u, 17565u, 17833u, 18105u, 18382u, 18663u, 18950u, 19241u, 19537u, 19838u,
    20144u, 20456u, 20773u, 21095u, 21423u, 21757u, 22097u, 22443u, 22795u, 23153u, 23518u, 23889u,
    24267u, 24652u, 25045u, 25444u, 25851u, 26266u, 26689u, 27120u, 27559u, 28006u, 28463u, 28928u,
    29403u, 29887u, 30381u, 30885u, 31400u, 31925u, 32462u, 33010u, 33569u, 34141u, 34725u, 35322u,
    35933u, 36557u, 37195u, 37849u, 38518u, 39202u, 39903u, 40621u, 41357u, 42111u, 42885u, 43678u,
    44492u, 45327u, 46185u, 47067u, 47973u, 48904u, 49862u, 50848u, 51863u, 52909u, 53987u, 55099u,
    56247u, 57431u, 58655u, 59921u, 61230u, 62585u, 63990u, 65446u, 66957u, 68527u, 70159u, 71857u,
    73626u, 75470u, 77395u, 79407u, 81512u, 83718u, 86031u, 88462u, 91019u, 93714u, 96559u, 99568u,
    102755u, 106140u, 109741u, 113581u, 117688u, 122090u, 126823u, 131929u, 137453u, 143454u, 149999u, 157168u,
    165059u, 173792u, 183513u, 194406u, 206704u, 220707u, 236803u, 255514u, 277547u, 303890u, 335970u, 375921u,
    427082u,
}};

std::uint32_t cutoffTanQ16(std::int32_t cents) {
    const std::int32_t clamped = std::clamp(
        cents,
        kSf2MinFilterCutoffCents,
        kSf2MaxFilterCutoffCents);
    const std::int32_t offset = clamped - kSf2MinFilterCutoffCents;
    const std::size_t index = static_cast<std::size_t>(offset / kTableStepCents);
    const std::int32_t remainder = offset % kTableStepCents;
    if (index + 1u >= kTanCutoffQ16.size() || remainder == 0) {
        return kTanCutoffQ16[index];
    }

    const std::uint64_t a = kTanCutoffQ16[index];
    const std::uint64_t b = kTanCutoffQ16[index + 1u];
    return static_cast<std::uint32_t>(
        (a * static_cast<std::uint64_t>(kTableStepCents - remainder) +
         b * static_cast<std::uint64_t>(remainder) +
         static_cast<std::uint64_t>(kTableStepCents / 2)) /
        static_cast<std::uint64_t>(kTableStepCents));
}

std::int32_t ratioQ24(std::int64_t numerator_q32, std::uint64_t denominator_q32) {
    if (denominator_q32 == 0) {
        return 0;
    }
    const bool negative = numerator_q32 < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-numerator_q32)
        : static_cast<std::uint64_t>(numerator_q32);
    const std::uint64_t scaled = magnitude << voice::kFilterCoefficientFractionBits;
    const std::uint64_t rounded =
        (scaled + denominator_q32 / 2u) / denominator_q32;
    const std::int64_t signed_value = negative
        ? -static_cast<std::int64_t>(rounded)
        : static_cast<std::int64_t>(rounded);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        signed_value,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

}  // namespace

std::int32_t velocityToFilterCutoffCents(
    std::uint8_t velocity,
    std::int32_t amount_cents) {
    const std::uint32_t clamped = std::min<std::uint32_t>(velocity, 127u);
    if (clamped >= 127u || amount_cents == 0) {
        return 0;
    }
    const std::uint32_t factor = clamped == 0u ? 128u : 128u - clamped;
    const std::int64_t scaled = static_cast<std::int64_t>(amount_cents) * factor;
    if (scaled >= 0) {
        return static_cast<std::int32_t>((scaled + 64) / 128);
    }
    return -static_cast<std::int32_t>((-scaled + 64) / 128);
}

bool prepareLowpassQ24(
    std::int32_t cutoff_cents,
    std::int32_t resonance_cb,
    std::uint32_t output_sample_rate,
    voice::BiquadLowpassConfig* output) {
    if (output == nullptr || output_sample_rate != kSf2FilterOutputSampleRate ||
        resonance_cb != 0) {
        return false;
    }

    const std::uint64_t k_q16 = cutoffTanQ16(cutoff_cents);
    const std::uint64_t one_q32 = std::uint64_t{1} << 32u;
    const std::uint64_t k2_q32 = k_q16 * k_q16;
    const std::uint64_t k_qinv_q32 = k_q16 << 16u;  // QInv=1 for 0 cB resonance.
    const std::uint64_t denominator_q32 = one_q32 + k_qinv_q32 + k2_q32;

    voice::BiquadLowpassConfig candidate{};
    candidate.enabled = true;
    candidate.a0_q24 = ratioQ24(
        static_cast<std::int64_t>(k2_q32),
        denominator_q32);
    candidate.a1_q24 = ratioQ24(
        static_cast<std::int64_t>(2u * k2_q32),
        denominator_q32);
    candidate.b1_q24 = ratioQ24(
        static_cast<std::int64_t>(2) *
            (static_cast<std::int64_t>(k2_q32) - static_cast<std::int64_t>(one_q32)),
        denominator_q32);
    candidate.b2_q24 = ratioQ24(
        static_cast<std::int64_t>(one_q32) -
            static_cast<std::int64_t>(k_qinv_q32) +
            static_cast<std::int64_t>(k2_q32),
        denominator_q32);

    if (!candidate.valid()) {
        return false;
    }
    *output = candidate;
    return true;
}

}  // namespace gm_synth::sf2
