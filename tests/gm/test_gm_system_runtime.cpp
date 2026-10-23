#include "gm/MidiChannelState.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ProgramChangeMailbox.h"
#include "gm/RealtimeProgramChangeHandoff.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "gm/ZoneResolver.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/VoiceRenderer.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ProgramChangeMailbox;
using gm_synth::gm::ProgramChangeRequest;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::RealtimeProgramChangeRouter;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::kGmPercussionChannel;
using gm_synth::gm::kSf2PercussionBank;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::voice::VoicePool;

RealtimeEvent cc(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ControlChange;
    event.channel = channel;
    event.data1 = controller;
    event.data2 = value;
    return event;
}

RealtimeEvent program(std::uint8_t channel, std::uint8_t value) {
    RealtimeEvent event{};
    event.type = EventType::ProgramChange;
    event.channel = channel;
    event.data1 = value;
    return event;
}

void testChannel10AlwaysPublishesSoundFontPercussionBank() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier events(programs);
    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramChangeRouter router(events, handoff);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    // Even arbitrary MIDI Bank Select values do not turn GM Channel 10 into a
    // melodic SoundFont bank. The raw state is preserved, but Program Change
    // resolves to SF2 percussion bank 128.
    assert(router.apply(cc(kGmPercussionChannel, 0, 3), voices));
    assert(router.apply(cc(kGmPercussionChannel, 32, 5), voices));
    assert(events.channelState(kGmPercussionChannel).selectedBank() == ((3u << 7u) | 5u));
    assert(events.channelStates().programBank(kGmPercussionChannel) == kSf2PercussionBank);

    assert(router.apply(program(kGmPercussionChannel, 7), voices));
    ProgramChangeRequest request{};
    assert(handoff.takeLatest(kGmPercussionChannel, &request));
    assert(request.channel == kGmPercussionChannel);
    assert(request.bank == kSf2PercussionBank);
    assert(request.program == 7);
}

void testGmResetRestoresAllChannelsAndPublishesProgramZero() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier events(programs);
    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramChangeRouter router(events, handoff);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    assert(router.apply(cc(2, 0, 4), voices));
    assert(router.apply(cc(2, 7, 12), voices));
    assert(router.apply(cc(2, 10, 1), voices));
    assert(router.apply(cc(2, 11, 2), voices));
    assert(router.apply(cc(2, 64, 127), voices));
    assert(router.apply(cc(2, 91, 90), voices));
    assert(router.apply(cc(2, 93, 80), voices));
    assert(router.apply(program(2, 40), voices));
    ProgramChangeRequest discarded{};
    assert(handoff.takeLatest(2, &discarded));

    RealtimeEvent reset{};
    reset.type = EventType::GmReset;
    reset.channel = 0;
    assert(router.apply(reset, voices));

    const auto& melodic = events.channelState(2);
    assert(melodic.selectedBank() == 0);
    assert(melodic.program == 0);
    assert(melodic.volume == 100);
    assert(melodic.pan == 64);
    assert(melodic.expression == 127);
    assert(!melodic.sustainDown());
    assert(melodic.reverb_send == 0);
    assert(melodic.chorus_send == 0);
    assert(melodic.pitch_bend == melodic.kPitchBendCenter);
    assert(melodic.pitch_bend_range_semitones == 2);
    assert(melodic.pitch_bend_range_cents == 0);
    assert(melodic.rpnNull());

    const auto& percussion = events.channelState(kGmPercussionChannel);
    assert(percussion.selectedBank() == kSf2PercussionBank);
    assert(percussion.program == 0);
    assert(events.channelStates().programBank(kGmPercussionChannel) == kSf2PercussionBank);

    ProgramChangeMailbox mailbox;
    assert(handoff.drainToMailbox(mailbox) == 16);
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        ProgramChangeRequest request{};
        assert(mailbox.take(channel, &request));
        assert(request.program == 0);
        assert(request.bank == (channel == kGmPercussionChannel ? kSf2PercussionBank : 0));
        assert(request.token != 0);
    }

    const auto stats = router.stats();
    assert(stats.gm_reset_events == 1);
    assert(stats.gm_reset_program_requests == 16);
    assert(stats.requests_rejected == 0);
}

void testChannel10NeverFallsThroughToOldMelodicSnapshot() {
    PlaybackZone zone{};
    PlaybackProgramSnapshot melodic{};
    melodic.channel = kGmPercussionChannel;
    melodic.bank = 0;
    melodic.program = 0;
    melodic.token = 1;
    melodic.bank_generation = 1;
    melodic.zones = &zone;
    melodic.zone_count = 1;
    assert(melodic.valid());

    PlaybackProgramSnapshotExchange programs;
    assert(programs.setInitialActive(&melodic));
    RealtimeProgramEventApplier events(programs);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    RealtimeEvent note{};
    note.type = EventType::NoteOn;
    note.channel = kGmPercussionChannel;
    note.data1 = 36;
    note.data2 = 100;
    assert(events.apply(note, voices));
    assert(voices.activeVoiceCount() == 0);
    assert(events.stats().percussion_program_unavailable == 1);
}

}  // namespace

int main() {
    testChannel10AlwaysPublishesSoundFontPercussionBank();
    testGmResetRestoresAllChannelsAndPublishesProgramZero();
    testChannel10NeverFallsThroughToOldMelodicSnapshot();
    std::cout << "GM system reset and percussion routing checks passed\n";
    return 0;
}
