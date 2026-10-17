#pragma once

#include "Sf2PanMath.h"
#include "Sf2ResolvedZone.h"
#include "../gm/PlaybackProgramSnapshot.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::sf2 {

constexpr std::uint16_t kSf2SampleTypeMask = 0x7fffu;
constexpr std::uint16_t kSf2MonoSampleType = 1u;
constexpr std::uint16_t kSf2RightSampleType = 2u;
constexpr std::uint16_t kSf2LeftSampleType = 4u;
constexpr std::uint16_t kSf2LinkedSampleType = 8u;

inline std::uint16_t baseSampleType(std::uint16_t sample_type) {
    return sample_type & kSf2SampleTypeMask;
}

inline bool isStereoSideSample(std::uint16_t sample_type) {
    const std::uint16_t type = baseSampleType(sample_type);
    return type == kSf2LeftSampleType || type == kSf2RightSampleType;
}

inline bool isUndefinedLinkedSample(std::uint16_t sample_type) {
    return baseSampleType(sample_type) == kSf2LinkedSampleType;
}

enum class StereoPairStatus : std::uint8_t {
    NotStereo = 0,
    Ok,
    UnsupportedLinked,
    InvalidPair,
};

struct StereoPairExpansion {
    NativeResolvedZone left{};
    NativeResolvedZone right{};
};

inline Sf2PdtaReader::SampleHeader sampleHeaderForNative(const NativeResolvedZone& zone) {
    Sf2PdtaReader::SampleHeader header{};
    header.start = zone.shdr_start;
    header.end = zone.shdr_end;
    header.start_loop = zone.shdr_loop_start;
    header.end_loop = zone.shdr_loop_end;
    header.sample_rate = zone.sample_rate;
    header.original_pitch = zone.sample_root_key;
    header.pitch_correction = zone.sample_pitch_correction_cents;
    header.sample_link = zone.sample_link;
    header.sample_type = zone.sample_type;
    return header;
}

inline bool stereoHeadersSynchronous(
    const Sf2PdtaReader::SampleHeader& left,
    const Sf2PdtaReader::SampleHeader& right) {
    if (left.sample_rate == 0 || left.sample_rate != right.sample_rate ||
        left.end <= left.start || right.end <= right.start ||
        left.end - left.start != right.end - right.start) {
        return false;
    }
    const std::int64_t left_loop_start =
        static_cast<std::int64_t>(left.start_loop) - left.start;
    const std::int64_t right_loop_start =
        static_cast<std::int64_t>(right.start_loop) - right.start;
    const std::int64_t left_loop_end =
        static_cast<std::int64_t>(left.end_loop) - left.start;
    const std::int64_t right_loop_end =
        static_cast<std::int64_t>(right.end_loop) - right.start;
    return left_loop_start == right_loop_start && left_loop_end == right_loop_end;
}

inline void refreshExpandedAddressFlags(
    std::uint64_t smpl_point_count,
    NativeResolvedZone* zone) {
    if (zone == nullptr) {
        return;
    }
    zone->flags &= ~(NativeZoneFlagSampleAddressOutOfSmpl |
                     NativeZoneFlagAddressOutsideSampleHeader |
                     NativeZoneFlagInvalidLoopRange |
                     NativeZoneFlagStereoOrLinkedSample |
                     NativeZoneFlagRomSample);
    const std::int64_t points = smpl_point_count >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(smpl_point_count);
    if (zone->sample_start < 0 || zone->sample_end > points ||
        zone->sample_start >= zone->sample_end) {
        zone->flags |= NativeZoneFlagSampleAddressOutOfSmpl;
    }
    if (zone->sample_start < static_cast<std::int64_t>(zone->shdr_start) ||
        zone->sample_end > static_cast<std::int64_t>(zone->shdr_end)) {
        zone->flags |= NativeZoneFlagAddressOutsideSampleHeader;
    }
    if ((zone->sampleModes() & 0x3) != 0 &&
        !(zone->sample_start <= zone->loop_start &&
          zone->loop_start < zone->loop_end &&
          zone->loop_end <= zone->sample_end)) {
        zone->flags |= NativeZoneFlagInvalidLoopRange;
    }
}

inline NativeResolvedZone expandedStereoSide(
    const NativeResolvedZone& source,
    std::uint16_t sample_id,
    const Sf2PdtaReader::SampleHeader& sample,
    const Sf2PdtaReader::SampleHeader& right_pitch_source,
    std::uint64_t smpl_point_count) {
    NativeResolvedZone side = source;
    const std::int64_t start_offset =
        source.sample_start - static_cast<std::int64_t>(source.shdr_start);
    const std::int64_t end_offset =
        source.sample_end - static_cast<std::int64_t>(source.shdr_end);
    const std::int64_t loop_start_offset =
        source.loop_start - static_cast<std::int64_t>(source.shdr_loop_start);
    const std::int64_t loop_end_offset =
        source.loop_end - static_cast<std::int64_t>(source.shdr_loop_end);

    side.sample_id = sample_id;
    side.shdr_start = sample.start;
    side.shdr_end = sample.end;
    side.shdr_loop_start = sample.start_loop;
    side.shdr_loop_end = sample.end_loop;
    side.sample_start = static_cast<std::int64_t>(sample.start) + start_offset;
    side.sample_end = static_cast<std::int64_t>(sample.end) + end_offset;
    side.loop_start = static_cast<std::int64_t>(sample.start_loop) + loop_start_offset;
    side.loop_end = static_cast<std::int64_t>(sample.end_loop) + loop_end_offset;
    side.sample_link = sample.sample_link;
    side.sample_type = sample.sample_type;

    // SF2 stereo pairs are pitch-controlled by the right sample. Keeping the
    // same rate/root/correction on both expanded voices guarantees synchronous
    // phase advancement while their sample address windows remain channel-local.
    side.sample_rate = right_pitch_source.sample_rate;
    side.sample_root_key = right_pitch_source.original_pitch;
    side.sample_pitch_correction_cents = right_pitch_source.pitch_correction;
    refreshExpandedAddressFlags(smpl_point_count, &side);
    return side;
}

inline StereoPairStatus expandStereoPair(
    const NativeResolvedZone& source,
    std::uint16_t linked_sample_id,
    const Sf2PdtaReader::SampleHeader& linked,
    std::uint64_t smpl_point_count,
    StereoPairExpansion* output) {
    if (output == nullptr) {
        return StereoPairStatus::InvalidPair;
    }
    const std::uint16_t source_type = baseSampleType(source.sample_type);
    if (source_type == kSf2MonoSampleType) {
        return StereoPairStatus::NotStereo;
    }
    if (source_type == kSf2LinkedSampleType) {
        return StereoPairStatus::UnsupportedLinked;
    }
    if (source_type != kSf2LeftSampleType && source_type != kSf2RightSampleType) {
        return StereoPairStatus::InvalidPair;
    }
    if ((source.sample_type & 0x8000u) != 0 || linked.isRom() ||
        linked_sample_id != source.sample_link || linked.sample_link != source.sample_id) {
        return StereoPairStatus::InvalidPair;
    }

    const std::uint16_t linked_type = baseSampleType(linked.sample_type);
    const bool source_left = source_type == kSf2LeftSampleType;
    if ((source_left && linked_type != kSf2RightSampleType) ||
        (!source_left && linked_type != kSf2LeftSampleType)) {
        return StereoPairStatus::InvalidPair;
    }

    const Sf2PdtaReader::SampleHeader source_header = sampleHeaderForNative(source);
    const Sf2PdtaReader::SampleHeader& left_header = source_left ? source_header : linked;
    const Sf2PdtaReader::SampleHeader& right_header = source_left ? linked : source_header;
    if (!stereoHeadersSynchronous(left_header, right_header)) {
        return StereoPairStatus::InvalidPair;
    }

    const std::uint16_t left_id = source_left ? source.sample_id : linked_sample_id;
    const std::uint16_t right_id = source_left ? linked_sample_id : source.sample_id;
    output->left = expandedStereoSide(
        source,
        left_id,
        left_header,
        right_header,
        smpl_point_count);
    output->right = expandedStereoSide(
        source,
        right_id,
        right_header,
        right_header,
        smpl_point_count);
    return StereoPairStatus::Ok;
}

inline std::int32_t recoverStaticAmplitudeQ15(const gm::PlaybackZone& zone) {
    const std::int32_t pan = zone.note_on_gain.pan_sf2_units;
    const std::int32_t left_pan = sf2PanChannelGainQ15(pan, true);
    const std::int32_t right_pan = sf2PanChannelGainQ15(pan, false);
    const bool use_left = left_pan >= right_pan;
    const std::int32_t pan_gain = use_left ? left_pan : right_pan;
    const std::int32_t channel_gain = use_left ? zone.gain_left_q15 : zone.gain_right_q15;
    if (pan_gain <= 0 || channel_gain <= 0) {
        return 0;
    }
    const std::int64_t numerator =
        static_cast<std::int64_t>(channel_gain) * voice::kQ15Unity + pan_gain / 2;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        numerator / pan_gain,
        0,
        voice::kQ15Unity));
}

inline std::int32_t multiplyStereoQ15(std::int32_t a, std::int32_t b) {
    if (a <= 0 || b <= 0) {
        return 0;
    }
    if (a >= voice::kQ15Unity) {
        return std::min<std::int32_t>(b, voice::kQ15Unity);
    }
    if (b >= voice::kQ15Unity) {
        return std::min<std::int32_t>(a, voice::kQ15Unity);
    }
    const std::int64_t product = static_cast<std::int64_t>(a) * b;
    return static_cast<std::int32_t>((product + (std::int64_t{1} << 14u)) >> 15u);
}

inline void applyStereoBasePan(gm::PlaybackZone* zone, bool left_channel) {
    if (zone == nullptr || !zone->note_on_gain.enabled) {
        return;
    }
    const std::int32_t amplitude = recoverStaticAmplitudeQ15(*zone);
    const std::int32_t effective_pan =
        static_cast<std::int32_t>(zone->note_on_gain.pan_sf2_units) +
        (left_channel ? -500 : 500);
    zone->note_on_gain.pan_sf2_units = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(effective_pan, -1000, 1000));
    zone->gain_left_q15 = multiplyStereoQ15(
        amplitude,
        sf2PanChannelGainQ15(effective_pan, true));
    zone->gain_right_q15 = multiplyStereoQ15(
        amplitude,
        sf2PanChannelGainQ15(effective_pan, false));
}

}  // namespace gm_synth::sf2
