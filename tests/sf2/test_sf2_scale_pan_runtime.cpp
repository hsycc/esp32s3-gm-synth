#include "../../firmware/gm/MidiChannelState.h"
#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/gm/ZoneResolver.h"
#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/sf2/Sf2PanMath.h"
#include "../../firmware/sf2/Sf2StereoPair.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::NativeZoneFlagAddressOutsideSampleHeader;
using gm_synth::sf2::NativeZoneFlagInvalidLoopRange;
using gm_synth::sf2::NativeZoneFlagSampleAddressOutOfSmpl;
using gm_synth::sf2::NativeZoneFlagStereoOrLinkedSample;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::StereoPairExpansion;
using gm_synth::sf2::StereoPairStatus;
using gm_synth::sf2::applyStereoBasePan;
using gm_synth::sf2::channelNoteOnGainQ15;
using gm_synth::sf2::controllerToPanSf2Units;
using gm_synth::sf2::expandStereoPair;
using gm_synth::sf2::kSf2LeftSampleType;
using gm_synth::sf2::kSf2LinkedSampleType;
using gm_synth::sf2::kSf2RightSampleType;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

PlaybackProgramSnapshot programFor(
    const PlaybackZone* zones,
    std::uint16_t zone_count = 1) {
    PlaybackProgramSnapshot program{};
    program.channel = 2;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = 1;
    program.zones = zones;
    program.zone_count = zone_count;
    return program;
}

PlaybackZone tunedZone(std::uint16_t cents_per_key) {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = cents_per_key != 0;
    zone.scale_tuning_cents_per_key = cents_per_key;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 0;
    zone.note_on_gain.expression_to_attenuation_cb = 0;
    zone.note_on_gain.pan_amount = 500;
    return zone;
}

void testFiftyCentsPerKeyIsAppliedOnBothResolverPaths() {
    PlaybackZone zone = tunedZone(50);
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch legacy{};
    assert(resolver.resolveNoteOn(program, 2, 72, 100, 1, &legacy) == ZoneResolveStatus::Ok);
    assert(legacy.count == 1);
    const std::uint64_t scaled = ZoneResolver::transposeCents(kPhaseOneQ32, 600);
    assert(legacy.voices[0].phase_increment_q32 == scaled);

    MidiChannelState state{};
    state.resetAll();
    state.pitch_bend = 16383;
    ResolvedVoiceBatch controlled{};
    assert(resolver.resolveNoteOn(program, state, 2, 72, 100, 2, &controlled) ==
           ZoneResolveStatus::Ok);
    assert(controlled.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(scaled, 200));
    assert(controlled.voices[0].realtime_control.unbent_phase_increment_q32 == scaled);
}

void testFullScaleTuningRangeRemainsRepresentable() {
    PlaybackZone octave_per_key = tunedZone(1200);
    const PlaybackProgramSnapshot program = programFor(&octave_per_key);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 61, 100, 3, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 1200));

    PlaybackZone fixed = tunedZone(0);
    assert(!fixed.key_tracking);
    const PlaybackProgramSnapshot fixed_program = programFor(&fixed);
    assert(resolver.resolveNoteOn(fixed_program, 2, 96, 100, 4, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 == kPhaseOneQ32);
}

void testDefaultCc10AndEqualPowerPanAreConsistent() {
    assert(controllerToPanSf2Units(0) == -500);
    assert(controllerToPanSf2Units(64) == 0);
    assert(controllerToPanSf2Units(127) == 492);
    assert(sf2PanChannelGainQ15(0, true) == 23170);
    assert(sf2PanChannelGainQ15(0, false) == 23170);

    std::int32_t left = 0;
    std::int32_t right = 0;
    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        64, 500,
        127, 0,
        &left, &right);
    assert(left == 23170);
    assert(right == 23170);

    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        0, 500,
        127, 0,
        &left, &right);
    assert(left == kQ15Unity);
    assert(right == 0);

    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        127, 500,
        127, 0,
        &left, &right);
    assert(left == sf2PanChannelGainQ15(492, true));
    assert(right == sf2PanChannelGainQ15(492, false));
}

NativeResolvedZone stereoLeftSource() {
    NativeResolvedZone source{};
    source.sample_id = 10;
    source.sample_link = 11;
    source.sample_type = kSf2LeftSampleType;
    source.shdr_start = 100;
    source.shdr_end = 200;
    source.shdr_loop_start = 120;
    source.shdr_loop_end = 180;
    source.sample_start = 102;
    source.sample_end = 197;
    source.loop_start = 121;
    source.loop_end = 178;
    source.sample_rate = 44100;
    source.sample_root_key = 60;
    source.sample_pitch_correction_cents = 4;
    source.flags = NativeZoneFlagStereoOrLinkedSample;
    source.generators.set(54, 1);
    return source;
}

Sf2PdtaReader::SampleHeader stereoRightHeader() {
    Sf2PdtaReader::SampleHeader right{};
    right.start = 300;
    right.end = 400;
    right.start_loop = 320;
    right.end_loop = 380;
    right.sample_rate = 44100;
    right.original_pitch = 64;
    right.pitch_correction = -7;
    right.sample_link = 10;
    right.sample_type = kSf2RightSampleType;
    return right;
}

void testStereoPairExpansionUsesRightPitchAndMirrorsAddressOffsets() {
    const NativeResolvedZone source = stereoLeftSource();
    const Sf2PdtaReader::SampleHeader right = stereoRightHeader();
    StereoPairExpansion expansion{};
    assert(expandStereoPair(source, 11, right, 1000, &expansion) == StereoPairStatus::Ok);

    assert(expansion.left.sample_id == 10);
    assert(expansion.right.sample_id == 11);
    assert(expansion.left.sample_start == 102);
    assert(expansion.left.sample_end == 197);
    assert(expansion.left.loop_start == 121);
    assert(expansion.left.loop_end == 178);
    assert(expansion.right.sample_start == 302);
    assert(expansion.right.sample_end == 397);
    assert(expansion.right.loop_start == 321);
    assert(expansion.right.loop_end == 378);

    // SoundFont stereo pitch is controlled by the right sample. The left voice
    // deliberately keeps its own PCM address window but adopts the right pitch
    // metadata so both phase accumulators advance synchronously.
    assert(expansion.left.sample_rate == 44100);
    assert(expansion.right.sample_rate == 44100);
    assert(expansion.left.sample_root_key == 64);
    assert(expansion.right.sample_root_key == 64);
    assert(expansion.left.sample_pitch_correction_cents == -7);
    assert(expansion.right.sample_pitch_correction_cents == -7);

    const std::uint32_t bad_flags =
        NativeZoneFlagStereoOrLinkedSample |
        NativeZoneFlagSampleAddressOutOfSmpl |
        NativeZoneFlagAddressOutsideSampleHeader |
        NativeZoneFlagInvalidLoopRange;
    assert((expansion.left.flags & bad_flags) == 0);
    assert((expansion.right.flags & bad_flags) == 0);
}

void testStereoPairValidationRejectsUndefinedAndBrokenLinks() {
    NativeResolvedZone source = stereoLeftSource();
    Sf2PdtaReader::SampleHeader right = stereoRightHeader();
    StereoPairExpansion expansion{};

    right.sample_link = 99;
    assert(expandStereoPair(source, 11, right, 1000, &expansion) ==
           StereoPairStatus::InvalidPair);

    right = stereoRightHeader();
    right.end = 401;
    assert(expandStereoPair(source, 11, right, 1000, &expansion) ==
           StereoPairStatus::InvalidPair);

    source.sample_type = kSf2LinkedSampleType;
    assert(expandStereoPair(source, 11, right, 1000, &expansion) ==
           StereoPairStatus::UnsupportedLinked);
}

void testExpandedStereoPanPreservesLayerBaseAcrossLiveCc10() {
    PlaybackZone left = tunedZone(100);
    PlaybackZone right = tunedZone(100);
    left.sample_id = 10;
    right.sample_id = 11;
    left.gain_left_q15 = 23170;
    left.gain_right_q15 = 23170;
    right.gain_left_q15 = 23170;
    right.gain_right_q15 = 23170;
    applyStereoBasePan(&left, true);
    applyStereoBasePan(&right, false);

    assert(left.note_on_gain.pan_sf2_units == -500);
    assert(right.note_on_gain.pan_sf2_units == 500);
    assert(left.gain_left_q15 == kQ15Unity);
    assert(left.gain_right_q15 == 0);
    assert(right.gain_left_q15 == 0);
    assert(right.gain_right_q15 == kQ15Unity);
    assert(left.valid() && right.valid());

    const std::array<PlaybackZone, 2> zones{left, right};
    const PlaybackProgramSnapshot program = programFor(zones.data(), zones.size());
    assert(program.valid());

    MidiChannelState state{};
    state.resetAll();
    state.pan = 0;
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 5, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.count == 2);
    assert(batch.voices[0].gain_left_q15 == kQ15Unity);
    assert(batch.voices[0].gain_right_q15 == 0);
    assert(batch.voices[1].gain_left_q15 == 23170);
    assert(batch.voices[1].gain_right_q15 == 23170);

    // With an already-left-panned stereo zone, the left layer must retain
    // -1000 before CC10 is added. Clamping it early to -500 would incorrectly
    // let a later rightward CC10 move that layer to center.
    PlaybackZone far_left = tunedZone(100);
    far_left.note_on_gain.pan_sf2_units = -500;
    far_left.gain_left_q15 = kQ15Unity;
    far_left.gain_right_q15 = 0;
    applyStereoBasePan(&far_left, true);
    assert(far_left.note_on_gain.pan_sf2_units == -1000);
    assert(far_left.valid());

    std::int32_t out_left = 0;
    std::int32_t out_right = 0;
    channelNoteOnGainQ15(
        0, 0, 100,
        far_left.note_on_gain.pan_sf2_units,
        127, 0,
        127, 500,
        127, 0,
        &out_left,
        &out_right);
    assert(out_left == kQ15Unity);
    assert(out_right == 0);
}

}  // namespace

int main() {
    testFiftyCentsPerKeyIsAppliedOnBothResolverPaths();
    testFullScaleTuningRangeRemainsRepresentable();
    testDefaultCc10AndEqualPowerPanAreConsistent();
    testStereoPairExpansionUsesRightPitchAndMirrorsAddressOffsets();
    testStereoPairValidationRejectsUndefinedAndBrokenLinks();
    testExpandedStereoPanPreservesLayerBaseAcrossLiveCc10();
    std::cout << "SF2 scale-tuning/pan/stereo runtime checks passed\n";
    return 0;
}
