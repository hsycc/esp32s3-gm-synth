#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "sample/PublishedSampleDirectory.h"
#include "sf2/Sf2PlaybackMaterializer.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueFilterPending;
using gm_synth::sf2::PlaybackIssueLfoPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoicePool;
using gm_synth::voice::kPhaseOneQ32;

NativeResolvedZone baseNativeZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 0;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.sample_start = 0;
    zone.sample_end = 256;
    zone.loop_start = 0;
    zone.loop_end = 256;
    zone.shdr_start = 0;
    zone.shdr_end = 256;
    zone.shdr_loop_start = 0;
    zone.shdr_loop_end = 256;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    zone.generators.set(54, 1);
    return zone;
}

void testMaterializerCarriesAllModLfoDestinations() {
    NativeResolvedZone native = baseNativeZone();
    native.generators.set(5, 100);    // modLfoToPitch
    native.generators.set(10, 1200);  // modLfoToFilterFc
    native.generators.set(13, 120);   // modLfoToVolume
    native.generators.set(21, -1200); // delayModLFO = 0.5 s
    native.generators.set(22, 0);     // freqModLFO = 8.176 Hz

    PlaybackZone zone{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(
        native,
        ResolvedZoneModulation{},
        &zone);

    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueLfoPending));
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(zone.mod_lfo.enabled);
    assert(zone.mod_lfo.pitch_depth_cents == 100);
    assert(zone.mod_lfo.filter_depth_cents == 1200);
    assert(zone.mod_lfo.volume_depth_cb == 120);
    assert(zone.mod_lfo.delay_frames == 22050);
    assert(zone.mod_lfo.phase_step_q32 != 0);
    assert(zone.valid());
}

PlaybackZone modLfoZone() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.loop_mode = LoopMode::Continuous;

    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 120;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 960;
    zone.note_on_gain.pan_amount = 1000;
    zone.note_on_gain.expression_to_attenuation_cb = 0;
    zone.note_on_gain.pan_sf2_units = 0;

    zone.note_on_filter.enabled = true;
    zone.note_on_filter.initial_cutoff_cents = 9000;
    zone.note_on_filter.velocity_to_cutoff_cents = 0;
    zone.note_on_filter.resonance_cb = 0;

    zone.mod_lfo.enabled = true;
    zone.mod_lfo.delay_frames = 0;
    zone.mod_lfo.phase_step_q32 = std::uint32_t{1} << 26u; // 1/64 cycle/frame
    zone.mod_lfo.pitch_depth_cents = 100;
    zone.mod_lfo.filter_depth_cents = 1200;
    zone.mod_lfo.volume_depth_cb = 120;
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

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 256> pcm{};

    PublishedFixture() {
        for (std::size_t index = 0; index < pcm.size(); ++index) {
            pcm[index] = static_cast<std::int16_t>((index % 32u) * 500 - 7000);
        }
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm.data();
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = 0;
        record.descriptor.loop_end = pcm.size();
        assert(directory.publish(0, &record));
    }
};

void testOneTriangleModulatesPitchFilterAndVolumeTogether() {
    PublishedFixture fixture;
    PlaybackZone zone = modLfoZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    MidiChannelState state{};
    state.resetAll();
    state.volume = 127;
    state.expression = 127;
    state.pan = 64;

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 1, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].mod_lfo.enabled);
    assert(batch.voices[0].mod_lfo.gain_table.enabled);
    assert(batch.voices[0].dynamic_lowpass_table.enabled);

    Voice voice;
    assert(voice.start(fixture.directory, batch.voices[0]));
    const std::uint64_t base_phase = voice.phaseIncrementQ32();
    const std::int32_t base_gain = voice.gainLeftQ15();
    const std::int32_t base_filter_a0 = voice.lowpassConfig().a0_q24;
    assert(base_phase == kPhaseOneQ32);
    assert(base_gain > 0 && base_gain < 32767);
    assert(base_filter_a0 > 0);

    // Control tick at frame 16 observes +1.0 triangle level. Per SF2 semantics
    // positive modLfoToVolume raises volume, i.e. subtracts attenuation.
    std::array<std::int32_t, 34> first_quarter{};
    assert(voice.renderAdd(first_quarter.data(), 17).frames_rendered == 17);
    assert(voice.currentPhaseIncrementQ32() > base_phase);
    assert(voice.currentGainLeftQ15() > base_gain);
    assert(voice.currentGainRightQ15() > base_gain);
    assert(voice.currentLowpassConfig().a0_q24 > base_filter_a0);

    // Ticks at frame 32 and 48 move through zero to -1.0. The negative half
    // cycle adds attenuation, lowers pitch and lowers the filter cutoff.
    std::array<std::int32_t, 64> third_quarter{};
    assert(voice.renderAdd(third_quarter.data(), 32).frames_rendered == 32);
    assert(voice.currentPhaseIncrementQ32() < base_phase);
    assert(voice.currentGainLeftQ15() < base_gain);
    assert(voice.currentGainRightQ15() < base_gain);
    assert(voice.currentLowpassConfig().a0_q24 < base_filter_a0);
}

RealtimeEvent noteOn() {
    RealtimeEvent event{};
    event.type = EventType::NoteOn;
    event.channel = 2;
    event.data1 = 60;
    event.data2 = 100;
    return event;
}

RealtimeEvent volume(std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = 2;
    event.data1 = 7;
    event.data2 = value;
    return event;
}

struct GainSnapshot {
    bool seen = false;
    std::int32_t base_gain = 0;
    std::int32_t current_gain = 0;
    bool table_enabled = false;
};

bool captureGain(void* opaque, Voice& voice) {
    auto* snapshot = static_cast<GainSnapshot*>(opaque);
    if (snapshot == nullptr || snapshot->seen) {
        return false;
    }
    snapshot->seen = true;
    snapshot->base_gain = voice.gainLeftQ15();
    snapshot->current_gain = voice.currentGainLeftQ15();
    snapshot->table_enabled = voice.modLfoConfig().gain_table.enabled;
    return false;
}

GainSnapshot activeGain(VoicePool& voices) {
    GainSnapshot snapshot{};
    voices.updateActiveVoices(2, &captureGain, &snapshot);
    assert(snapshot.seen);
    return snapshot;
}

void testLiveChannelVolumeRebuildsSoundingModLfoGainCurve() {
    PublishedFixture fixture;
    PlaybackZone zone = modLfoZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    PlaybackProgramSnapshotExchange programs;
    assert(programs.setInitialActive(&program));
    VoicePool voices{fixture.directory};
    RealtimeProgramEventApplier applier{programs};

    assert(applier.apply(volume(127), voices));
    assert(applier.apply(noteOn(), voices));
    assert(voices.activeVoiceCount() == 1);

    // Move the Mod-LFO to its positive peak so the live table point is not the
    // center value, then lower CC7. The entire nonlinear curve must be rebuilt.
    std::array<std::int32_t, 34> output{};
    voices.renderAdd(output.data(), 17);
    const GainSnapshot before = activeGain(voices);
    assert(before.table_enabled);

    assert(applier.apply(volume(80), voices));
    const GainSnapshot after = activeGain(voices);
    assert(after.table_enabled);
    assert(after.base_gain < before.base_gain);
    assert(after.current_gain < before.current_gain);
}

void testDelayKeepsAllThreeDestinationsAtBaseUntilElapsed() {
    PublishedFixture fixture;
    PlaybackZone zone = modLfoZone();
    zone.mod_lfo.delay_frames = 8;
    const PlaybackProgramSnapshot program = programFor(&zone);

    MidiChannelState state{};
    state.resetAll();
    state.volume = 127;
    state.expression = 127;

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 2, &batch) ==
           ZoneResolveStatus::Ok);

    Voice voice;
    assert(voice.start(fixture.directory, batch.voices[0]));
    const std::uint64_t base_phase = voice.phaseIncrementQ32();
    const std::int32_t base_gain = voice.gainLeftQ15();
    const std::int32_t base_filter_a0 = voice.lowpassConfig().a0_q24;

    std::array<std::int32_t, 16> output{};
    assert(voice.renderAdd(output.data(), 8).frames_rendered == 8);
    assert(voice.currentPhaseIncrementQ32() == base_phase);
    assert(voice.currentGainLeftQ15() == base_gain);
    assert(voice.currentLowpassConfig().a0_q24 == base_filter_a0);
}

}  // namespace

int main() {
    testMaterializerCarriesAllModLfoDestinations();
    testOneTriangleModulatesPitchFilterAndVolumeTogether();
    testLiveChannelVolumeRebuildsSoundingModLfoGainCurve();
    testDelayKeepsAllThreeDestinationsAtBaseUntilElapsed();
    std::cout << "SF2 Mod-LFO runtime checks passed\n";
    return 0;
}