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

using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::ProgramChangeMailbox;
using gm_synth::gm::ProgramChangeRequest;
using gm_synth::gm::RealtimeProgramChangeHandoff;
using gm_synth::gm::RealtimeProgramChangeRouter;
using gm_synth::gm::RealtimeProgramEventApplier;
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

void testBankSelectIsCapturedOnlyWhenProgramChangeArrives() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier events(programs);
    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramChangeRouter router(events, handoff);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    assert(router.apply(cc(2, 0, 3), voices));
    assert(router.apply(cc(2, 32, 5), voices));
    assert(events.channelState(2).selectedBank() == ((3u << 7u) | 5u));

    ProgramChangeRequest none{};
    assert(!handoff.takeLatest(2, &none));

    assert(router.apply(program(2, 40), voices));
    ProgramChangeRequest request{};
    assert(handoff.takeLatest(2, &request));
    assert(request.channel == 2);
    assert(request.bank == ((3u << 7u) | 5u));
    assert(request.program == 40);
    assert(request.token != 0);
    assert(events.channelState(2).program == 40);
}

void testRapidProgramChangesCoalesceToLatestPerChannel() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier events(programs);
    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramChangeRouter router(events, handoff);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    assert(router.apply(program(0, 16), voices));
    assert(router.apply(program(0, 48), voices));
    assert(router.apply(program(0, 56), voices));
    assert(router.apply(program(1, 24), voices));

    ProgramChangeMailbox mailbox;
    assert(handoff.drainToMailbox(mailbox) == 2);

    ProgramChangeRequest ch0{};
    ProgramChangeRequest ch1{};
    assert(mailbox.take(0, &ch0));
    assert(mailbox.take(1, &ch1));
    assert(ch0.program == 56);
    assert(ch1.program == 24);
    assert(ch0.token != 0 && ch1.token != 0 && ch0.token != ch1.token);

    const auto handoff_stats = handoff.stats();
    assert(handoff_stats.published == 4);
    assert(handoff_stats.superseded >= 2);
    assert(handoff_stats.taken == 2);
    assert(handoff_stats.rejected == 0);

    const auto router_stats = router.stats();
    assert(router_stats.program_change_events == 4);
    assert(router_stats.requests_published == 4);
    assert(router_stats.requests_rejected == 0);
}

void testNonProgramEventsStillUseExistingRealtimeApplier() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier events(programs);
    RealtimeProgramChangeHandoff handoff;
    RealtimeProgramChangeRouter router(events, handoff);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    assert(router.apply(cc(3, 7, 77), voices));
    assert(events.channelState(3).volume == 77);
    ProgramChangeRequest request{};
    assert(!handoff.takeLatest(3, &request));
}

}  // namespace

int main() {
    testBankSelectIsCapturedOnlyWhenProgramChangeArrives();
    testRapidProgramChangesCoalesceToLatestPerChannel();
    testNonProgramEventsStillUseExistingRealtimeApplier();
    std::cout << "realtime Program Change handoff checks passed\n";
    return 0;
}
