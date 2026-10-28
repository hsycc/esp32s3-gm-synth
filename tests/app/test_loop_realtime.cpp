#include "app/LoopRealtimeSession.h"
#include "loop_analysis_fixture.h"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace gm_synth;
using Session = app::LoopRealtimeSession<>;
using Status = app::LoopStartupStatus;
using Service = gm::ProgramChangeServiceStatus;
using Event = realtime::EventType;
using Phase = decltype(std::declval<Session>().phase());

namespace {
std::atomic<unsigned> checks{0};
void require(bool ok, int line) {
    checks.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        std::fprintf(stderr, "REALTIME_HOST_FAIL line=%d\n", line);
        std::abort();
    }
}
#define CHECK(value) require(bool(value), __LINE__)

enum class Domain { Control, Audio };
thread_local Domain domain = Domain::Control;
struct DomainScope {
    Domain previous;
    explicit DomainScope(Domain next) : previous(domain) { domain = next; }
    ~DomainScope() { domain = previous; }
};

bool hash(const std::uint8_t* bytes, std::size_t size,
          app::VerifiedReadOnlyBank::Digest& digest) noexcept {
    CHECK(domain == Domain::Control);
    return SHA256(bytes, size, digest.data()) != nullptr;
}

template<class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

// These callbacks execute inside the real resident analyzer and verified-reader
// control path. Pausing never mutates a snapshot, PCM, or the provider itself.
struct Checkpoint {
    std::atomic<bool> armed{false}, entered{false}, resume{false}, cancel{false};
    std::atomic<unsigned> calls{0}, io_calls{0};
    static bool analysis(void* opaque) {
        auto& self = *static_cast<Checkpoint*>(opaque);
        CHECK(domain == Domain::Control);
        self.calls.fetch_add(1, std::memory_order_relaxed);
        if (self.armed.load(std::memory_order_acquire)) {
            self.entered.store(true, std::memory_order_release);
            await([&] { return self.resume.load(std::memory_order_acquire); });
        }
        return !self.cancel.load(std::memory_order_acquire);
    }
    static bool io(void* opaque) {
        auto& self = *static_cast<Checkpoint*>(opaque);
        CHECK(domain == Domain::Control);
        self.io_calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void pause() {
        entered.store(false, std::memory_order_relaxed);
        resume.store(false, std::memory_order_relaxed);
        armed.store(true, std::memory_order_release);
    }
    void release() { resume.store(true, std::memory_order_release); }
};

struct Backing {
    void* arena = nullptr;
    std::uint8_t* bytes = nullptr;
    unsigned releases = 0;
    std::atomic<bool> may_release{true};
    Session* observer = nullptr;

    app::StartupLease lease(std::size_t capacity) {
        return leaseBytes(loop_analysis_fixture::bytes, sizeof(loop_analysis_fixture::bytes), capacity);
    }
    app::StartupLease leaseBytes(const std::uint8_t* source, std::size_t size, std::size_t capacity) {
        CHECK(!arena && !bytes);
        arena = std::malloc(capacity);
        bytes = static_cast<std::uint8_t*>(std::malloc(size));
        CHECK(arena && bytes);
        std::memcpy(bytes, source, size);
        return {bytes, size, arena, capacity, this, &release};
    }
    static void release(void* opaque) noexcept {
        auto& self = *static_cast<Backing*>(opaque);
        CHECK(self.may_release.load(std::memory_order_acquire));
        // The callback observes an already-destroyed graph. It may not be used
        // to read a running VoicePool or to force another task to stop.
        if (self.observer) {
            CHECK(self.observer->audioVoices() == 0);
            CHECK(self.observer->audioLoops() == 0);
        }
        std::free(self.bytes);
        std::free(self.arena);
        self.bytes = nullptr;
        self.arena = nullptr;
        ++self.releases;
    }
    ~Backing() { CHECK(!arena && !bytes); }
};

struct Fixture {
    Backing backing;
    Checkpoint checkpoint;
    app::StartupSerials serials;
    // Caller-owned workspace storage must outlive both actors and the Session.
    std::unique_ptr<Session::Storage> storage{new Session::Storage};
    std::unique_ptr<Session> session{new Session(*storage)};
    std::array<std::int16_t, 128> pcm{};
    bool begun = false;

    explicit Fixture(bool enabled = true, std::size_t capacity = 20 * 1024) {
        backing.observer = session.get();
        app::LoopRealtimeConfig config;
        config.analysis.experimental_opt_in = enabled;
        config.analysis.checkpoint = &Checkpoint::analysis;
        config.analysis.checkpoint_context = &checkpoint;
        config.io_checkpoint = &Checkpoint::io;
        config.io_context = &checkpoint;
        CHECK(session->start(backing.lease(capacity), loop_analysis_fixture::digest,
                             hash, serials, config) == Status::Running);
        CHECK((session->controlStage().admitted > 0) == enabled);
        CHECK(backing.releases == 0);
    }
    ~Fixture() {
        // Every test joins its worker before reaching this destructor. Fail
        // here instead of hiding a missing explicit actor shutdown in a test.
        CHECK(backing.releases == 1);
        session.reset();
        storage.reset();
    }
    void begin() {
        CHECK(!begun);
        CHECK(session->beginActors());
        begun = true;
        backing.may_release.store(false, std::memory_order_release);
    }
    void push(Event type, unsigned a, unsigned b = 0, unsigned offset = 0) {
        // This helper is called by the audio owner between render calls, or
        // before actors start. The queue still has exactly one producer.
        realtime::RealtimeEvent event{};
        event.timestamp_frame = session->audioFrame() + offset;
        event.type = type;
        event.channel = 0;
        event.data1 = static_cast<std::uint8_t>(a);
        event.data2 = static_cast<std::uint8_t>(b);
        CHECK(session->enqueue(event));
    }
    void render(unsigned blocks = 1) {
        DomainScope scope(Domain::Audio);
        for (unsigned i = 0; i < blocks; ++i) CHECK(session->audioRender(pcm.data()));
    }
    Service service() {
        DomainScope scope(Domain::Control);
        return session->controlService();
    }
    void finish() {
        session->requestStop();
        CHECK(!session->stopQuiescent());
        CHECK(backing.releases == 0);
        {
            DomainScope scope(Domain::Audio);
            session->audioStop();
        }
        CHECK(session->audioStopped());
        if (!session->controlStopped()) {
            CHECK(!session->stopQuiescent());
            DomainScope scope(Domain::Control);
            session->controlStop();
        }
        CHECK(session->controlStopped());
        // No worker may access Session from this point. Post-join reports are
        // safe, unlike polling ordinary renderer/provider fields during work.
        CHECK(session->publicationStats().stale_release_faults == 0);
        CHECK(session->queueStats().rejected_full == 0);
        CHECK(session->queueStats().rejected_out_of_order == 0);
        backing.may_release.store(true, std::memory_order_release);
        CHECK(session->stopQuiescent());
        CHECK(backing.releases == 1);
        CHECK(session->stopQuiescent());
        CHECK(backing.releases == 1);
        CHECK(!session->audioRender(pcm.data()));
        CHECK(!session->enqueue(realtime::RealtimeEvent{}));
    }
};

void disabled_default() {
    Fixture f(false);
    f.begin();
    f.push(Event::NoteOn, 61, 100);
    f.render();
    CHECK(f.session->audioVoices() == 1 && f.session->audioLoops() == 0);
    f.push(Event::NoteOff, 61);
    f.render(128);
    CHECK(f.session->audioVoices() == 0);
    CHECK(f.session->controlAnalysis().analyses == 0);
    CHECK(f.session->audioStats().events_rejected == 0);
    f.finish();
    std::puts("REALTIME_HOST_CASE default_off PASS");
}

void continuous_real_provider() {
    Fixture f;
    f.begin();
    // key61 is the unchanged fixture's UntilRelease zone. Sustain holds its
    // sounding loop across NoteOff and the subsequent Program Change.
    f.push(Event::NoteOn, 61, 100);
    f.push(Event::ControlChange, 64, 127, 16);
    f.push(Event::NoteOff, 61, 0, 32);
    f.render();
    CHECK(f.session->audioVoices() == 1 && f.session->audioLoops() == 1);
    f.push(Event::ProgramChange, 1, 0, 8);
    f.render();
    f.checkpoint.pause();
    Service prepared = Service::NoPending;
    std::thread control([&] { prepared = f.service(); });
    await([&] { return f.checkpoint.entered.load(std::memory_order_acquire); });
    CHECK(f.session->phase() == Phase::Preparing);
    const auto before = f.session->audioFrame();
    const auto io_before = f.checkpoint.io_calls.load(std::memory_order_relaxed);
    unsigned sounded = 0;
    for (unsigned i = 0; i < 24; ++i) {
        f.render();
        CHECK(f.session->audioProgram() == 0);
        CHECK(f.session->phase() == Phase::Preparing);
        CHECK(f.session->audioLoops() == 1);
        for (const auto value : f.pcm) if (value) ++sounded;
    }
    CHECK(f.session->audioFrame() == before + 24 * 64);
    CHECK(sounded > 0);
    CHECK(f.checkpoint.io_calls.load(std::memory_order_relaxed) == io_before);
    CHECK(!f.session->stopQuiescent() && f.backing.releases == 0);
    f.checkpoint.release();
    control.join();
    CHECK(prepared == Service::Ok && f.session->phase() == Phase::Ready);
    CHECK(f.session->audioProgram() == 0);
    f.render();
    CHECK(f.session->audioProgram() == 1 && f.session->phase() == Phase::Committed);
    CHECK(f.service() == Service::NoPending);
    CHECK(f.session->phase() == Phase::Idle);
    // Retirement has released old plan/Program pins, but this sustained Voice
    // still owns sample0 and its already-copied loop playback state.
    CHECK(f.session->audioLoops() == 1 && f.session->audioVoices() == 1);
    f.push(Event::NoteOn, 60, 100);
    f.render();
    CHECK(f.session->audioVoices() == 2 && f.session->audioLoops() == 1);
    f.push(Event::ControlChange, 64, 0, 16);
    f.push(Event::ControlChange, 21, 100, 32);
    f.render(128);
    CHECK(f.session->audioLoops() == 0 && f.session->audioVoices() == 1);
    f.push(Event::ControlChange, 120, 0);
    f.push(Event::ProgramChange, 0, 0, 8);
    f.render();
    CHECK(f.service() == Service::Ok);
    f.render();
    CHECK(f.session->audioProgram() == 0);
    CHECK(f.service() == Service::NoPending);
    f.push(Event::NoteOn, 61, 100);
    f.render(2);
    CHECK(f.session->audioLoops() == 1);
    f.push(Event::NoteOff, 61, 0, 20);
    f.render(128);
    CHECK(f.session->audioVoices() == 0 && f.session->audioLoops() == 0);
    CHECK(f.session->audioStats().events_applied == 11);
    CHECK(f.session->audioStats().events_rejected == 0);
    CHECK(f.session->controlAnalysis().completed > 0);
    f.finish();
    std::puts("REALTIME_HOST_CASE continuous_real_provider overlapped_blocks=24 midi_events=11 PASS");
}

void missing_program() {
    Fixture f;
    f.begin();
    f.push(Event::NoteOn, 60, 100);
    f.push(Event::ProgramChange, 127, 0, 8);
    f.render();
    CHECK(f.service() == Service::PresetUnavailable);
    CHECK(f.session->phase() == Phase::Idle);
    CHECK(f.service() == Service::NoPending);
    f.render(32);
    CHECK(f.session->audioProgram() == 0 && f.session->audioLoops() == 1);
    CHECK(f.session->audioStats().events_rejected == 0);
    f.finish();
    std::puts("REALTIME_HOST_CASE missing_program_keeps_audio PASS");
}

void residency_failure() {
    // One 8,320-byte sample fits. The new Program needs another sample while
    // the active Program and sounding Voice retain the original one.
    Fixture f(true, 10 * 1024);
    f.begin();
    f.push(Event::NoteOn, 60, 100);
    f.push(Event::ProgramChange, 1, 0, 8);
    f.render();
    CHECK(f.service() == Service::ResidencyPrepareFailed);
    CHECK(f.session->phase() == Phase::Idle);
    f.render(32);
    CHECK(f.session->audioProgram() == 0 && f.session->audioLoops() == 1);
    CHECK(f.session->controlResidencyStats().used_bytes <= 10 * 1024);
    f.finish();
    std::puts("REALTIME_HOST_CASE residency_failure_keeps_audio PASS");
}

void stop_transition(bool commit) {
    Fixture f;
    f.begin();
    f.push(Event::NoteOn, 61, 100);
    f.push(Event::ProgramChange, 1, 0, 8);
    f.render();
    CHECK(f.service() == Service::Ok && f.session->phase() == Phase::Ready);
    if (commit) {
        f.render();
        CHECK(f.session->phase() == Phase::Committed);
    }
    f.finish();
    std::puts(commit ? "REALTIME_HOST_CASE stop_committed_before_ack PASS" :
                       "REALTIME_HOST_CASE stop_ready_before_commit PASS");
}

void stop_during_prepare() {
    Fixture f;
    f.begin();
    f.push(Event::NoteOn, 61, 100);
    f.push(Event::ProgramChange, 1, 0, 8);
    f.render();
    f.checkpoint.pause();
    std::thread control([&] {
        (void)f.service();
        f.session->controlStop();
    });
    await([&] { return f.checkpoint.entered.load(std::memory_order_acquire); });
    f.render(8);
    CHECK(f.session->audioProgram() == 0 && f.session->audioLoops() == 1);
    f.session->requestStop();
    CHECK(f.session->stopRequested());
    CHECK(!f.session->stopQuiescent());
    {
        DomainScope audio(Domain::Audio);
        f.session->audioStop();
    }
    CHECK(f.session->audioStopped() && !f.session->controlStopped());
    CHECK(!f.session->stopQuiescent() && f.backing.releases == 0);
    f.checkpoint.release();
    control.join();
    CHECK(f.session->controlStopped());
    CHECK(f.session->publicationStats().stale_release_faults == 0);
    f.backing.may_release.store(true, std::memory_order_release);
    CHECK(f.session->stopQuiescent() && f.backing.releases == 1);
    CHECK(f.session->stopQuiescent() && f.backing.releases == 1);
    std::puts("REALTIME_HOST_CASE stop_during_real_analysis stopped_actors=2 control_joined=1 PASS");
}

void cancelled_analysis() {
    Fixture f;
    f.begin();
    f.push(Event::NoteOn, 61, 100);
    f.push(Event::ProgramChange, 1, 0, 8);
    f.render();
    CHECK(f.service() == Service::Ok);
    f.render();
    CHECK(f.service() == Service::NoPending);
    f.push(Event::ControlChange, 120, 0);
    f.push(Event::ProgramChange, 0, 0, 8);
    f.render();
    f.checkpoint.cancel.store(true, std::memory_order_release);
    CHECK(f.service() == Service::Ok);
    CHECK(f.session->controlStage().admitted == 0);
    f.render();
    CHECK(f.session->audioProgram() == 0);
    CHECK(f.service() == Service::NoPending);
    f.push(Event::NoteOn, 61, 100);
    f.render();
    CHECK(f.session->audioVoices() == 1 && f.session->audioLoops() == 0);
    CHECK(f.session->audioStats().events_rejected == 0);
    f.finish();
    std::puts("REALTIME_HOST_CASE cancelled_analysis_direct_fallback PASS");
}

void storage_exclusive_reuse() {
    auto storage = std::make_unique<Session::Storage>();
    app::StartupSerials serials;
    Backing first, rejected, next;
    auto active = std::make_unique<Session>(*storage);
    auto blocked = std::make_unique<Session>(*storage);
    first.observer = active.get();
    rejected.observer = blocked.get();
    app::LoopRealtimeConfig config;
    config.analysis.experimental_opt_in = true;
    CHECK(active->start(first.lease(20 * 1024), loop_analysis_fixture::digest,
                        hash, serials, config) == Status::Running);
    const auto initial_serial = active->serial();
    CHECK(active->beginActors());
    first.may_release.store(false, std::memory_order_release);
    realtime::RealtimeEvent note{};
    note.type = Event::NoteOn; note.channel = 0; note.data1 = 61; note.data2 = 100;
    CHECK(active->enqueue(note));
    std::array<std::int16_t, 128> pcm{};
    CHECK(active->audioRender(pcm.data()));
    CHECK(active->audioLoops() == 1);

    // A second session may verify its own bank, but must not replace the
    // allocator or scratch still owned by the first generation's live actors.
    CHECK(blocked->start(rejected.lease(20 * 1024), loop_analysis_fixture::digest,
                         hash, serials, config) == Status::ArenaFailed);
    CHECK(rejected.releases == 1 && first.releases == 0);
    blocked.reset();
    CHECK(active->audioRender(pcm.data()));
    CHECK(active->audioProgram() == 0 && active->audioLoops() == 1);
    active->requestStop();
    active->audioStop();
    CHECK(!active->stopQuiescent() && first.releases == 0);
    active->controlStop();
    first.may_release.store(true, std::memory_order_release);
    CHECK(active->stopQuiescent() && first.releases == 1);

    // The storage identity is reusable only after whole-group shutdown. The
    // newly constructed session receives a fresh serial even though pointers
    // to the two generation arrays have the same addresses as before.
    auto replacement = std::make_unique<Session>(*storage);
    next.observer = replacement.get();
    CHECK(replacement->start(next.lease(20 * 1024), loop_analysis_fixture::digest,
                             hash, serials, config) == Status::Running);
    CHECK(replacement->serial() > initial_serial);
    CHECK(replacement->beginActors());
    next.may_release.store(false, std::memory_order_release);
    CHECK(replacement->enqueue(note));
    CHECK(replacement->audioRender(pcm.data()));
    CHECK(replacement->audioLoops() == 1);
    replacement->requestStop();
    replacement->audioStop(); replacement->controlStop();
    CHECK(replacement->publicationStats().stale_release_faults == 0);
    next.may_release.store(true, std::memory_order_release);
    CHECK(replacement->stopQuiescent() && next.releases == 1);
    replacement.reset(); active.reset(); storage.reset();
    std::puts("REALTIME_HOST_CASE storage_exclusive_then_reusable PASS");
}

void real_bank_cohort(const char* path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    CHECK(data.size() == 5898214);
    app::VerifiedReadOnlyBank::Digest digest{};
    CHECK(hash(data.data(), data.size(), digest));
    std::array<char, 65> hex{};
    for (std::size_t i = 0; i < digest.size(); ++i)
        std::snprintf(hex.data() + i * 2, 3, "%02x", unsigned(digest[i]));
    CHECK(std::strcmp(hex.data(), "9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62") == 0);
    app::StartupSerials serials;
    for (bool enabled : {false, true}) {
        Backing backing;
        auto storage = std::make_unique<Session::Storage>();
        auto session = std::make_unique<Session>(*storage);
        backing.observer = session.get();
        app::LoopRealtimeConfig config;
        config.program = 16;
        config.analysis.experimental_opt_in = enabled;
        CHECK(session->start(backing.leaseBytes(data.data(), data.size(), 4718592), digest,
                             hash, serials, config) == Status::Running);
        const auto initial_stage = session->controlStage();
        const auto initial_analysis = session->controlAnalysis();
        CHECK((initial_stage.admitted > 0) == enabled);
        CHECK(initial_analysis.analysis.selected_fade == (enabled ? 32 : 0));
        CHECK(session->beginActors());
        backing.may_release.store(false, std::memory_order_release);
        auto schedule = [&](std::uint64_t frame, Event type, unsigned a, unsigned b = 0) {
            realtime::RealtimeEvent event{};
            event.timestamp_frame = frame;
            event.type = type; event.channel = 0;
            event.data1 = static_cast<std::uint8_t>(a);
            event.data2 = static_cast<std::uint8_t>(b);
            CHECK(session->enqueue(event));
        };
        std::array<std::int16_t, 128> pcm{};
        auto render = [&](unsigned blocks) {
            DomainScope audio(Domain::Audio);
            for (unsigned i = 0; i < blocks; ++i) CHECK(session->audioRender(pcm.data()));
        };
        auto renderUntil = [&](std::uint64_t frame) {
            CHECK(frame >= session->audioFrame() && frame % 64 == 0);
            render(static_cast<unsigned>((frame - session->audioFrame()) / 64));
        };
        // The physical profile's complete 16 -> 1 -> 16 script, with the
        // same MIDI values and offsets. Control service is serialized here;
        // true analyzer/render overlap is exercised by the separate cases.
        schedule(0, Event::ControlChange, 91, 40);
        schedule(0, Event::ControlChange, 93, 20);
        schedule(0, Event::ControlChange, 21, 0);
        schedule(0, Event::NoteOn, 60, 100);
        schedule(0, Event::NoteOn, 61, 100);
        schedule(32 * 64, Event::ControlChange, 64, 127);
        schedule(48 * 64 + 7, Event::NoteOff, 61);
        schedule(80 * 64, Event::ControlChange, 21, 64);
        schedule(96 * 64, Event::ControlChange, 64, 0);
        schedule(112 * 64, Event::ControlChange, 21, 127);
        schedule(128 * 64, Event::ProgramChange, 1);
        render(129);
        CHECK(session->controlService() == Service::Ok);
        render(1);
        CHECK(session->audioProgram() == 1);
        CHECK(session->controlService() == Service::NoPending);
        // Program16's held key60 survives retirement of its original plan and
        // Program pins. New Program1 events will subsequently release it.
        CHECK(session->audioVoices() > 0);
        CHECK((session->audioLoops() > 0) == enabled);
        const auto replacement_base = session->audioFrame() + 16 * 64;
        schedule(replacement_base, Event::NoteOn, 60, 100);
        schedule(replacement_base + 32 * 64 + 11, Event::NoteOff, 60);
        schedule(replacement_base + 64 * 64, Event::ProgramChange, 16);
        renderUntil(replacement_base + 65 * 64);
        CHECK(session->controlService() == Service::Ok);
        render(1);
        CHECK(session->audioProgram() == 16);
        CHECK(session->controlService() == Service::NoPending);
        const auto returned_base = session->audioFrame() + 16 * 64;
        schedule(returned_base, Event::NoteOn, 61, 100);
        schedule(returned_base + 16 * 64, Event::ControlChange, 64, 127);
        schedule(returned_base + 32 * 64 + 3, Event::NoteOff, 61);
        schedule(returned_base + 64 * 64, Event::ControlChange, 64, 0);
        schedule(returned_base + 80 * 64, Event::ControlChange, 21, 0);
        const auto onset = returned_base + 128 * 64;
        const auto first = onset + 64;
        const auto end = first + 4096 * 64;
        schedule(onset - 64, Event::ControlChange, 120, 0);
        for (unsigned key = 48; key < 72; ++key) schedule(onset, Event::NoteOn, key, 64);
        for (unsigned key = 48; key < 72; ++key) schedule(end, Event::NoteOff, key);
        renderUntil(first);
        const auto clipped_before = session->audioStats().clipped_samples;
        unsigned voices_min = 32, voices_max = 0, loops_min = 32, loops_max = 0;
        for (unsigned block = 0; block < 4096; ++block) {
            render(1);
            voices_min = std::min(voices_min, session->audioVoices());
            voices_max = std::max(voices_max, session->audioVoices());
            loops_min = std::min(loops_min, session->audioLoops());
            loops_max = std::max(loops_max, session->audioLoops());
        }
        const auto cohort_stats = session->audioStats();
        const auto cohort_clipped = cohort_stats.clipped_samples - clipped_before;
        std::printf("REALTIME_HOST_BANK analysis=%u program=16 cohort_blocks=4096 voices_min=%u voices_max=%u loops_min=%u loops_max=%u cohort_clipped=%llu global_clipped=%llu plans=%u last_fade=%u sha256=%s\n",
            unsigned(enabled), voices_min, voices_max, loops_min, loops_max,
            static_cast<unsigned long long>(cohort_clipped),
            static_cast<unsigned long long>(cohort_stats.clipped_samples), unsigned(initial_stage.admitted),
            unsigned(initial_analysis.analysis.selected_fade), hex.data());
        CHECK(voices_min == 24 && voices_max == 24);
        CHECK(loops_min == (enabled ? 24u : 0u) && loops_max == loops_min);
        CHECK(cohort_stats.output_frames == end && cohort_stats.events_applied == 44);
        CHECK(cohort_stats.events_rejected == 0);
        render(256); // queued, same-frame NoteOffs and the original FX/release tails
        CHECK(session->audioVoices() == 0);
        CHECK(session->audioStats().events_applied == 68 && session->audioStats().events_rejected == 0);
        const auto total_clipped = session->audioStats().clipped_samples;
        const bool headroom_pass = total_clipped == 0;
        std::printf("REALTIME_BANK_RESULT mode=%s voices=24 loops=%u clipped=%llu headroom_pass=%u physical_timing_approved=0\n",
            enabled ? "on" : "off", loops_min, static_cast<unsigned long long>(total_clipped), unsigned(headroom_pass));
        // This exact, unchanged 24-note workload is a known NEGATIVE headroom
        // qualification in both modes. A successful test means its clipping
        // correctly remains a rejection, never that hardware or headroom passed.
        // No volume, velocity, sample-Voice count, DSP, or physical gate is
        // reduced to make the workload look acceptable. Resolving this release
        // blocker requires a separately reviewed output-headroom design.
        CHECK(cohort_clipped > 0 && !headroom_pass);
        session->requestStop(); session->audioStop(); session->controlStop();
        CHECK(session->publicationStats().stale_release_faults == 0);
        backing.may_release.store(true, std::memory_order_release);
        CHECK(session->stopQuiescent() && backing.releases == 1);
        session.reset(); storage.reset();
    }
    std::puts("REALTIME_HOST_CASE real_fm_cohort_headroom_rejected PASS physical_timing_approved=0");
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    CHECK(argc == 2);
    disabled_default();
    continuous_real_provider();
    missing_program();
    residency_failure();
    stop_transition(false);
    stop_transition(true);
    stop_during_prepare();
    cancelled_analysis();
    storage_exclusive_reuse();
    real_bank_cohort(argv[1]);
    // Checks include bounded-wait iterations and therefore vary by scheduling.
    std::printf("REALTIME_HOST_RESULT suites=10 checks=%u PASS actor_ownership=1\n",
                checks.load(std::memory_order_relaxed));
}
