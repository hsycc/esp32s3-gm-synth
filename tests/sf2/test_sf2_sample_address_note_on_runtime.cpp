#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "sf2/Sf2PlaybackMaterializer.h"
#include "sf2/Sf2ZoneModulatorResolver.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueCustomModulatorsPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::addPreparedNoteOnModulator;
using gm_synth::sf2::evaluatePreparedNoteOnModulator;
using gm_synth::voice::LoopMode;

NativeResolvedZone baseNativeZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.shdr_start = 100;
    zone.shdr_end = 400;
    zone.shdr_loop_start = 180;
    zone.shdr_loop_end = 360;
    zone.sample_start = 180;  // static +80 sample-start generator offset
    zone.sample_end = 400;
    zone.loop_start = 180;
    zone.loop_end = 360;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    return zone;
}

PlaybackProgramSnapshot programFor(const PlaybackZone* zone) {
    PlaybackProgramSnapshot program{};
    program.channel = 2;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = 1;
    program.zones = zone;
    program.zone_count = 1;
    return program;
}

void testVelocitySampleStartOffsetMaterializesAndRunsAtNoteOn() {
    NativeResolvedZone native = baseNativeZone();
    ResolvedZoneModulation modulation{};
    const Sf2PreparedNoteOnModulator route{0x0002u, 0u, -100, 0u};
    assert(addPreparedNoteOnModulator(&modulation.prepared_note_on_routes, route));
    modulation.supported_prepared_note_on_records = 1;

    PlaybackZone zone{};
    const auto materialized = Sf2PlaybackMaterializer{}.materialize(
        native, modulation, &zone);
    assert(materialized.playablePreview());
    assert(!materialized.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.sample_region.enabled);
    assert(zone.sample_region.start_frame == 80u);
    assert(zone.sample_region.end_frame == 300u);
    assert(zone.prepared_note_on_modulators.count == 1u);
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    constexpr std::uint8_t velocity = 127;
    assert(resolver.resolveNoteOn(program, 2, 60, velocity, 1, &batch) ==
        ZoneResolveStatus::Ok);
    assert(batch.count == 1u);
    const std::int32_t delta = evaluatePreparedNoteOnModulator(
        route, 60, velocity);
    const std::uint32_t expected = static_cast<std::uint32_t>(
        std::max<std::int32_t>(0, 80 + delta));
    assert(batch.voices[0].sample_region.enabled);
    assert(batch.voices[0].sample_region.start_frame == expected);
    assert(batch.voices[0].sample_region.end_frame == 300u);
}

void testDynamicSampleStartClampsToRealizableLoopBoundary() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.root_phase_increment_q32 = gm_synth::voice::kPhaseOneQ32;
    zone.loop_mode = LoopMode::Continuous;
    zone.sample_region.enabled = true;
    zone.sample_region.start_frame = 50;
    zone.sample_region.end_frame = 300;
    zone.sample_region.loop_start_frame = 120;
    zone.sample_region.loop_end_frame = 260;
    const Sf2PreparedNoteOnModulator route{0x0002u, 0u, 10000, 0u};
    assert(addPreparedNoteOnModulator(&zone.prepared_note_on_modulators, route));
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 127, 2, &batch) ==
        ZoneResolveStatus::Ok);
    assert(batch.count == 1u);
    assert(batch.voices[0].sample_region.start_frame == 120u);
    assert(batch.voices[0].sample_region.loop_start_frame == 120u);
    assert(batch.voices[0].sample_region.loop_end_frame == 260u);
}

void testCoarseSampleStartUses32768PointGeneratorUnits() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.root_phase_increment_q32 = gm_synth::voice::kPhaseOneQ32;
    zone.sample_region.enabled = true;
    zone.sample_region.start_frame = 0;
    zone.sample_region.end_frame = 40000;
    const Sf2PreparedNoteOnModulator route{0x0002u, 4u, 1, 0u};
    assert(addPreparedNoteOnModulator(&zone.prepared_note_on_modulators, route));
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 127, 3, &batch) ==
        ZoneResolveStatus::Ok);
    assert(batch.count == 1u);
    const std::int32_t coarse = evaluatePreparedNoteOnModulator(route, 60, 127);
    assert(coarse == 1);
    assert(batch.voices[0].sample_region.start_frame == 32768u);
}

}  // namespace

int main() {
    testVelocitySampleStartOffsetMaterializesAndRunsAtNoteOn();
    testDynamicSampleStartClampsToRealizableLoopBoundary();
    testCoarseSampleStartUses32768PointGeneratorUnits();
    std::cout << "SF2 sample-address NoteOn runtime checks passed\n";
    return 0;
}
