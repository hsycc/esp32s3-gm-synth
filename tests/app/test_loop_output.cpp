#include "app/LoopRealtimeSession.h"
#include "app/LoopRealtimeReplay.h"
#include "app/OutputPeakProtector.h"
#include "loop_analysis_fixture.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gm_synth;
using Protector = app::OutputPeakProtector;
using OutputMode = Protector::Mode;
using Session = app::LoopRealtimeSession<>;
using Event = realtime::EventType;
using Service = gm::ProgramChangeServiceStatus;
using OutputStats = decltype(std::declval<Session>().audioOutputStats());

namespace {
constexpr std::uint32_t kUnity = UINT32_C(1) << 30;
constexpr const char* kBankSha = "9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62";
std::atomic<unsigned> checks{0};
void require(bool condition, int line) {
    checks.fetch_add(1, std::memory_order_relaxed);
    if (!condition) {
        std::fprintf(stderr, "OUTPUT_HOST_FAIL line=%d\n", line);
        std::abort();
    }
}
#define CHECK(condition) require(bool(condition), __LINE__)

using WideBlock = std::array<std::int32_t, 128>;
using PcmBlock = std::array<std::int16_t, 128>;

std::uint32_t magnitude(std::int32_t value) {
    return static_cast<std::uint32_t>(value < 0 ? -std::int64_t{value} : value);
}
bool overloaded(std::int32_t value) {
    return value < -32768 || value > 32767;
}
std::int16_t clipped(std::int32_t value) {
    return static_cast<std::int16_t>(std::max(-32768, std::min(32767, value)));
}
template<class Stats> bool sameStats(const Stats& a, const Stats& b) {
    return a.raw_peak_abs == b.raw_peak_abs &&
        a.raw_overload_samples == b.raw_overload_samples &&
        a.post_peak_abs == b.post_peak_abs &&
        a.post_hard_clipped == b.post_hard_clipped &&
        a.limited_frames == b.limited_frames &&
        a.gain_min_q30 == b.gain_min_q30 &&
        a.input_frames == b.input_frames &&
        a.output_frames == b.output_frames &&
        a.latency_frames == b.latency_frames;
}

void exact_bypass_and_master() {
    constexpr std::array<std::int32_t, 12> values{
        std::numeric_limits<std::int32_t>::min(), -65536, -32769, -32768,
        -32767, -1, 0, 1, 32767, 32768, 65536,
        std::numeric_limits<std::int32_t>::max()};
    for (int master : {0, 1, 12345, 32767}) {
        Protector processor(OutputMode::Bypass);
        WideBlock input{};
        PcmBlock output{};
        std::uint64_t overloads = 0;
        std::uint32_t peak = 0;
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = values[i % values.size()];
        CHECK(processor.process(input.data(), 64, master, output.data()));
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto value = master == 32767 ? input[i] :
                static_cast<std::int32_t>((std::int64_t{input[i]} * master) / 32767);
            CHECK(output[i] == clipped(value));
            peak = std::max(peak, magnitude(value));
            overloads += overloaded(value);
        }
        const auto stats = processor.stats();
        CHECK(stats.raw_peak_abs == peak && stats.raw_overload_samples == overloads);
        CHECK(stats.post_hard_clipped == overloads);
        CHECK(stats.latency_frames == 0 && stats.input_frames == 64 && stats.output_frames == 64);
        CHECK(stats.gain_min_q30 == kUnity && stats.limited_frames == 0);
        CHECK(!processor.flush(output.data()));
    }
    std::puts("OUTPUT_HOST_CASE exact_bypass_and_master PASS");
}

void delay_identity_and_drain() {
    Protector processor(OutputMode::Peak64);
    WideBlock input{};
    PcmBlock output{}, previous{};
    for (unsigned block = 0; block < 8; ++block) {
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<std::int32_t>((block * 7919u + i * 563u) % 60001u) - 30000;
        CHECK(processor.process(input.data(), 64, 32767, output.data()));
        CHECK(output == previous); // First block is the explicit 64-frame prime.
        for (std::size_t i = 0; i < input.size(); ++i) previous[i] = static_cast<std::int16_t>(input[i]);
    }
    CHECK(processor.stats().input_frames == 512 && processor.stats().output_frames == 512);
    CHECK(processor.flush(output.data()));
    CHECK(output == previous); // The last nonzero block cannot disappear at stop.
    const auto drained = processor.stats();
    CHECK(drained.input_frames == 512 && drained.output_frames == 576 && drained.latency_frames == 64);
    CHECK(drained.raw_overload_samples == 0 && drained.post_hard_clipped == 0);
    CHECK(drained.gain_min_q30 == kUnity && drained.limited_frames == 0);
    output.fill(123);
    CHECK(!processor.flush(output.data()));
    CHECK(std::all_of(output.begin(), output.end(), [](auto sample) { return sample == 123; }));
    CHECK(sameStats(processor.stats(), drained));
    CHECK(!processor.process(input.data(), 64, 32767, output.data()));
    CHECK(sameStats(processor.stats(), drained));
    std::puts("OUTPUT_HOST_CASE delay_identity_and_drain PASS");
}

void bounded_input_and_invalid_calls() {
    Protector processor(OutputMode::Peak64);
    WideBlock input{};
    PcmBlock output{};
    const auto before = processor.stats();
    CHECK(!processor.process(nullptr, 64, 32767, output.data()));
    CHECK(!processor.process(input.data(), 64, 32767, nullptr));
    CHECK(!processor.process(input.data(), 63, 32767, output.data()));
    CHECK(!processor.process(input.data(), 65, 32767, output.data()));
    CHECK(!processor.process(input.data(), 0, 32767, output.data()));
    CHECK(!processor.process(input.data(), 64, -1, output.data()));
    CHECK(!processor.process(input.data(), 64, 32768, output.data()));
    CHECK(!processor.flush(nullptr));
    CHECK(sameStats(before, processor.stats()));
    std::uint32_t random = 0x6a09e667u, expected_peak = 0;
    std::uint64_t expected_overloads = 0;
    for (unsigned block = 0; block < 256; ++block) {
        for (std::size_t i = 0; i < input.size(); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            input[i] = static_cast<std::int32_t>(std::int64_t{random} - (INT64_C(1) << 31));
        }
        input.front() = std::numeric_limits<std::int32_t>::min();
        input.back() = std::numeric_limits<std::int32_t>::max();
        const int master = block % 3 == 0 ? 1 : (block % 3 == 1 ? 12345 : 32767);
        for (auto sample : input) {
            const auto value = master == 32767 ? sample :
                static_cast<std::int32_t>((std::int64_t{sample} * master) / 32767);
            expected_peak = std::max(expected_peak, magnitude(value));
            expected_overloads += overloaded(value);
        }
        CHECK(processor.process(input.data(), 64, master, output.data()));
        for (auto sample : output) CHECK(sample >= -32767 && sample <= 32767);
    }
    CHECK(processor.flush(output.data()));
    for (auto sample : output) CHECK(sample >= -32767 && sample <= 32767);
    const auto stats = processor.stats();
    CHECK(stats.raw_peak_abs == UINT32_C(2147483648) && stats.raw_peak_abs == expected_peak);
    CHECK(stats.raw_overload_samples == expected_overloads && expected_overloads > 0);
    CHECK(stats.post_hard_clipped == 0 && stats.post_peak_abs <= 32767);
    CHECK(stats.gain_min_q30 > 0 && stats.gain_min_q30 < kUnity && stats.limited_frames > 0);
    CHECK(stats.input_frames == 256 * 64 && stats.output_frames == 257 * 64);
    std::puts("OUTPUT_HOST_CASE int32_extremes_invalid_calls PASS");
}

void stereo_linked_attack_and_release() {
    Protector processor(OutputMode::Peak64);
    WideBlock quiet{}, loud{};
    PcmBlock output{};
    for (unsigned frame = 0; frame < 64; ++frame) {
        quiet[frame * 2] = 16000; quiet[frame * 2 + 1] = -8000;
        loud[frame * 2] = 64000; loud[frame * 2 + 1] = -32000;
    }
    CHECK(processor.process(quiet.data(), 64, 32767, output.data()));
    CHECK(std::all_of(output.begin(), output.end(), [](auto sample) { return sample == 0; }));
    CHECK(processor.process(loud.data(), 64, 32767, output.data()));
    // The low-level block before an overload ramps down ahead of its arrival.
    // A single block-wide gain jump would fail this smoothness/property test.
    int prior = 16000;
    for (unsigned frame = 0; frame < 64; ++frame) {
        const int left = output[frame * 2], right = output[frame * 2 + 1];
        CHECK(left <= prior && prior - left <= 251);
        CHECK(std::abs(left + 2 * right) <= 1); // Shared gain, quantization only.
        prior = left;
    }
    CHECK(output[0] > output[126] && output[126] < 8200);
    CHECK(processor.process(quiet.data(), 64, 32767, output.data()));
    for (unsigned frame = 0; frame < 64; ++frame) {
        const int left = output[frame * 2], right = output[frame * 2 + 1];
        CHECK(left <= 32767 && left >= 32000);
        CHECK(std::abs(left + 2 * right) <= 1);
    }
    prior = 0;
    for (unsigned block = 0; block < 65; ++block) {
        CHECK(processor.process(quiet.data(), 64, 32767, output.data()));
        for (unsigned frame = 0; frame < 64; ++frame) {
            const int left = output[frame * 2], right = output[frame * 2 + 1];
            CHECK(left >= prior && left <= 16000);
            if (block || frame) CHECK(left - prior <= 5);
            CHECK(std::abs(left + 2 * right) <= 1);
            prior = left;
        }
    }
    CHECK(output[126] == 16000 && output[127] == -8000);
    CHECK(processor.stats().raw_overload_samples == 64);
    CHECK(processor.stats().post_hard_clipped == 0);
    CHECK(processor.flush(output.data()));
    std::puts("OUTPUT_HOST_CASE stereo_linked_smooth_attack_release PASS");
}

bool hash(const std::uint8_t* data, std::size_t bytes,
          app::VerifiedReadOnlyBank::Digest& digest) noexcept {
    return SHA256(data, bytes, digest.data()) != nullptr;
}
std::string sha256(const std::uint8_t* bytes, std::size_t size) {
    app::VerifiedReadOnlyBank::Digest digest{};
    CHECK(hash(bytes, size, digest));
    std::array<char, 65> hex{};
    for (std::size_t i = 0; i < digest.size(); ++i)
        std::snprintf(hex.data() + i * 2, 3, "%02x", unsigned(digest[i]));
    return hex.data();
}
std::uint64_t checksum(const std::vector<std::int16_t>& pcm) {
    std::uint64_t value = UINT64_C(14695981039346656037);
    for (auto sample : pcm) {
        const auto word = static_cast<std::uint16_t>(sample);
        value = (value ^ (word & 0xffu)) * UINT64_C(1099511628211);
        value = (value ^ (word >> 8)) * UINT64_C(1099511628211);
    }
    return value;
}
struct Backing {
    void* arena = nullptr;
    unsigned releases = 0;
    app::StartupLease lease(const std::vector<std::uint8_t>& bank) {
        arena = std::malloc(4718592);
        CHECK(arena);
        return {bank.data(), bank.size(), arena, 4718592, this, &release};
    }
    static void release(void* opaque) noexcept {
        auto& self = *static_cast<Backing*>(opaque);
        std::free(self.arena); self.arena = nullptr; ++self.releases;
    }
    ~Backing() { CHECK(!arena && releases == 1); }
};
struct SceneResult {
    bool loop = false;
    OutputMode mode = OutputMode::Bypass;
    std::vector<std::int16_t> pcm;
    OutputStats stats{};
    std::uint64_t cohort_overloads = 0;
    std::uint64_t event_count = 0;
    unsigned voices = 0, loops = 0;
};

// This is the original 68-event scene, including both Program Changes, the
// exact unchanged 24-Voice cohort and its 256-block release/FX tail. Serialized
// control service makes PCM comparisons reproducible; concurrent ownership is
// independently covered by test_loop_realtime.cpp and QEMU's real actors.
SceneResult real_scene(const std::vector<std::uint8_t>& bank, bool loop, OutputMode mode) {
    Backing backing;
    auto storage = std::make_unique<Session::Storage>();
    auto session = std::make_unique<Session>(*storage);
    app::VerifiedReadOnlyBank::Digest digest{};
    CHECK(hash(bank.data(), bank.size(), digest));
    app::StartupSerials serials;
    app::LoopRealtimeConfig config;
    config.program = 16; config.analysis.experimental_opt_in = loop; config.output_mode = mode;
    CHECK(session->start(backing.lease(bank), digest, hash, serials, config) == app::LoopStartupStatus::Running);
    CHECK((session->controlStage().admitted > 0) == loop);
    CHECK(session->controlAnalysis().analysis.selected_fade == (loop ? 32 : 0));
    CHECK(session->beginActors());
    SceneResult result; result.loop = loop; result.mode = mode;
    PcmBlock pcm{};
    auto schedule = [&](std::uint64_t frame, Event type, unsigned a, unsigned b = 0) {
        realtime::RealtimeEvent event{};
        event.timestamp_frame = frame; event.type = type; event.channel = 0;
        event.data1 = static_cast<std::uint8_t>(a); event.data2 = static_cast<std::uint8_t>(b);
        CHECK(session->enqueue(event));
    };
    auto render = [&](unsigned blocks) {
        for (unsigned block = 0; block < blocks; ++block) {
            CHECK(session->audioRender(pcm.data()));
            result.pcm.insert(result.pcm.end(), pcm.begin(), pcm.end());
            result.voices = std::max(result.voices, session->audioVoices());
            result.loops = std::max(result.loops, session->audioLoops());
        }
    };
    auto render_until = [&](std::uint64_t frame) {
        CHECK(frame >= session->audioFrame() && frame % 64 == 0);
        render(static_cast<unsigned>((frame - session->audioFrame()) / 64));
    };
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
    CHECK(session->audioVoices() > 0 && (session->audioLoops() > 0) == loop);
    const auto replacement_base = session->audioFrame() + 16 * 64;
    schedule(replacement_base, Event::NoteOn, 60, 100);
    schedule(replacement_base + 32 * 64 + 11, Event::NoteOff, 60);
    schedule(replacement_base + 64 * 64, Event::ProgramChange, 16);
    render_until(replacement_base + 65 * 64);
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
    render_until(first);
    const auto before = session->audioOutputStats().raw_overload_samples;
    for (unsigned block = 0; block < 4096; ++block) {
        render(1);
        CHECK(session->audioVoices() == 24 && session->audioLoops() == (loop ? 24u : 0u));
    }
    result.cohort_overloads = session->audioOutputStats().raw_overload_samples - before;
    CHECK(result.cohort_overloads == (loop ? 4445u : 9915u));
    CHECK(session->audioStats().output_frames == end && session->audioStats().events_applied == 44);
    render(256);
    CHECK(session->audioVoices() == 0 && session->audioLoops() == 0);
    CHECK(session->audioStats().events_applied == 68 && session->audioStats().events_rejected == 0);
    result.event_count = session->audioStats().events_applied;
    const auto input_frames = session->audioFrame();
    const auto before_drain = session->audioOutputStats();
    const bool drained = session->audioDrainOutput(pcm.data());
    CHECK(drained == (mode == OutputMode::Peak64));
    if (drained) result.pcm.insert(result.pcm.end(), pcm.begin(), pcm.end());
    CHECK(!session->audioDrainOutput(pcm.data()));
    CHECK(session->audioFrame() == input_frames);
    result.stats = session->audioOutputStats();
    CHECK(result.stats.raw_overload_samples == before_drain.raw_overload_samples);
    CHECK(result.stats.raw_overload_samples == (loop ? 4479u : 9950u));
    CHECK(result.stats.input_frames == input_frames && result.stats.output_frames * 2 == result.pcm.size());
    CHECK(result.stats.latency_frames == (mode == OutputMode::Peak64 ? 64u : 0u));
    CHECK(result.stats.output_frames == input_frames + result.stats.latency_frames);
    if (mode == OutputMode::Peak64) {
        CHECK(result.stats.post_hard_clipped == 0 && result.stats.post_peak_abs <= 32767);
        CHECK(result.stats.gain_min_q30 > 0 && result.stats.gain_min_q30 < kUnity);
        CHECK(result.stats.limited_frames > 0);
        CHECK(std::all_of(result.pcm.begin(), result.pcm.begin() + 128, [](auto sample) { return sample == 0; }));
    } else {
        CHECK(result.stats.post_hard_clipped == result.stats.raw_overload_samples);
        CHECK(result.stats.gain_min_q30 == kUnity && result.stats.limited_frames == 0);
    }
    session->requestStop(); session->audioStop(); session->controlStop();
    CHECK(session->publicationStats().stale_release_faults == 0);
    CHECK(session->stopQuiescent() && backing.releases == 1);
    CHECK(session->publicationStats().live_voice_refs == 0);
    session.reset(); storage.reset();
    return result;
}

void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    u16(bytes, static_cast<std::uint16_t>(value)); u16(bytes, static_cast<std::uint16_t>(value >> 16));
}
std::string write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm) {
    CHECK(pcm.size() * 2 + 36 <= std::numeric_limits<std::uint32_t>::max());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + pcm.size() * 2);
    auto tag = [&](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
    tag("RIFF"); u32(bytes, static_cast<std::uint32_t>(36 + pcm.size() * 2)); tag("WAVE");
    tag("fmt "); u32(bytes, 16); u16(bytes, 1); u16(bytes, 2); u32(bytes, 44100);
    u32(bytes, 44100 * 4); u16(bytes, 4); u16(bytes, 16);
    tag("data"); u32(bytes, static_cast<std::uint32_t>(pcm.size() * 2));
    for (auto sample : pcm) u16(bytes, static_cast<std::uint16_t>(sample));
    std::ofstream output(path, std::ios::binary);
    CHECK(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close(); CHECK(output.good());
    return sha256(bytes.data(), bytes.size());
}

void real_bank_matrix(const char* path, const char* output_directory) {
    std::ifstream input(path, std::ios::binary); CHECK(input.good());
    const std::vector<std::uint8_t> bank{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    CHECK(bank.size() == 5898214 && sha256(bank.data(), bank.size()) == kBankSha);
    std::ofstream manifest;
    if (output_directory) {
        std::filesystem::create_directories(output_directory);
        manifest.open(std::filesystem::path(output_directory) / "output-audition.json");
        CHECK(manifest.good());
        manifest << "{\n  \"schema\": \"v1-output-headroom-audition-1\",\n"
            << "  \"scene\": \"unchanged-fm16-68-event-24-voice-cohort\",\n"
            << "  \"bank_sha256\": \"" << kBankSha << "\",\n"
            << "  \"sample_rate_hz\": 44100, \"channels\": 2, \"bits_per_sample\": 16,\n"
            << "  \"physical_timing_approved\": false, \"listening_approved\": false,\n"
            << "  \"notes\": \"Candidate includes 64 leading silent frames and a drained final block. Align by latency for A/B. Bypass intentionally retains known overload. Host PCM is not physical or subjective approval.\",\n"
            << "  \"renders\": [\n";
    }
    bool comma = false;
    for (bool loop : {false, true}) {
        for (auto mode : {OutputMode::Bypass, OutputMode::Peak64}) {
            const auto result = real_scene(bank, loop, mode);
            const auto repeat = real_scene(bank, loop, mode);
            CHECK(result.pcm == repeat.pcm && sameStats(result.stats, repeat.stats));
            CHECK(result.voices == repeat.voices && result.loops == repeat.loops &&
                  result.cohort_overloads == repeat.cohort_overloads && result.event_count == repeat.event_count);
            const char* loop_name = loop ? "on" : "off";
            const char* mode_name = mode == OutputMode::Peak64 ? "peak" : "bypass";
            const auto& s = result.stats;
            const auto pcm_hash = checksum(result.pcm);
            std::printf("OUTPUT_BANK_RESULT loop=%s output=%s voices=%u loops=%u events=%llu cohort_raw_overload=%llu raw_overload_samples=%llu raw_peak_abs=%u post_hard_clipped=%llu post_peak_abs=%u input_frames=%llu output_frames=%llu latency_frames=%u gain_min_q30=%u limited_frames=%llu checksum=%llu deterministic=1 physical_timing_approved=0 listening_approved=0\n",
                loop_name, mode_name, result.voices, result.loops,
                static_cast<unsigned long long>(result.event_count),
                static_cast<unsigned long long>(result.cohort_overloads),
                static_cast<unsigned long long>(s.raw_overload_samples), unsigned(s.raw_peak_abs),
                static_cast<unsigned long long>(s.post_hard_clipped), unsigned(s.post_peak_abs),
                static_cast<unsigned long long>(s.input_frames), static_cast<unsigned long long>(s.output_frames),
                unsigned(s.latency_frames), unsigned(s.gain_min_q30),
                static_cast<unsigned long long>(s.limited_frames), static_cast<unsigned long long>(pcm_hash));
            if (output_directory) {
                const std::string filename = std::string("fm16-loop-") + loop_name + "-" + mode_name + ".wav";
                const auto wav_sha = write_wav(std::filesystem::path(output_directory) / filename, result.pcm);
                if (comma) manifest << ",\n";
                comma = true;
                manifest << "    {\"file\": \"" << filename << "\", \"sha256\": \"" << wav_sha
                    << "\", \"loop\": \"" << loop_name << "\", \"output\": \"" << mode_name
                    << "\", \"input_frames\": " << s.input_frames << ", \"output_frames\": " << s.output_frames
                    << ", \"latency_frames\": " << s.latency_frames << ", \"events\": " << result.event_count
                    << ", \"sample_voices\": " << result.voices << ", \"loop_voices\": " << result.loops
                    << ", \"cohort_raw_overload\": " << result.cohort_overloads
                    << ", \"raw_overload_samples\": " << s.raw_overload_samples << ", \"raw_peak_abs\": " << s.raw_peak_abs
                    << ", \"post_hard_clipped\": " << s.post_hard_clipped << ", \"post_peak_abs\": " << s.post_peak_abs
                    << ", \"gain_min_q30\": " << s.gain_min_q30 << ", \"limited_frames\": " << s.limited_frames
                    << ", \"pcm_checksum_fnv1a64\": " << pcm_hash << ", \"repeat_exact\": true}";
            }
        }
    }
    if (output_directory) { manifest << "\n  ]\n}\n"; manifest.close(); CHECK(manifest.good()); }
    std::puts("OUTPUT_HOST_CASE unchanged_real_fm_matrix_deterministic PASS physical_timing_approved=0 listening_approved=0");
}

struct ReplayResult : SceneResult {
    std::uint64_t trace = app::loop_realtime_replay::kHashOffset;
    unsigned channel_mask = 0, loop_channel_mask = 0, switches = 0;
    std::array<unsigned, 2> channel_voices{}, channel_loops{};
};

ReplayResult musical_replay(const std::vector<std::uint8_t>& bank, bool loop, OutputMode mode) {
    namespace replay = app::loop_realtime_replay;
    using MusicSession = app::LoopRealtimeSession<16, 8, 2>;
    using Phase = MusicSession::Phase;
    Backing backing;
    auto storage = std::make_unique<MusicSession::Storage>();
    auto session = std::make_unique<MusicSession>(*storage);
    app::StartupSerials serials;
    app::VerifiedReadOnlyBank::Digest digest{};
    CHECK(hash(bank.data(), bank.size(), digest));
    app::LoopRealtimeConfig config;
    config.program = 16; config.analysis.experimental_opt_in = loop; config.output_mode = mode;
    CHECK(session->start(backing.lease(bank), digest, hash, serials, config) == app::LoopStartupStatus::Running);
    for (unsigned channel = 0; channel < 2; ++channel) {
        CHECK(session->audioProgram(channel) == 16);
        CHECK((session->controlStage(channel).admitted > 0) == loop);
    }
    CHECK(session->beginActors());
    ReplayResult result; result.loop = loop; result.mode = mode;
    PcmBlock pcm{};
    auto render = [&](unsigned blocks) {
        for (unsigned block = 0; block < blocks; ++block) {
            CHECK(session->audioRender(pcm.data()));
            result.pcm.insert(result.pcm.end(), pcm.begin(), pcm.end());
            result.voices = std::max(result.voices, session->audioVoices());
            result.loops = std::max(result.loops, session->audioLoops());
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto voices = session->audioVoices(channel);
                const auto loops = session->audioLoops(channel);
                result.channel_voices[channel] = std::max(result.channel_voices[channel], voices);
                result.channel_loops[channel] = std::max(result.channel_loops[channel], loops);
                if (voices) result.channel_mask |= 1u << channel;
                if (loops) result.loop_channel_mask |= 1u << channel;
            }
        }
    };
    auto queue = [&](auto events, std::uint64_t base) {
        for (auto event : events) {
            event.timestamp_frame += base;
            CHECK(session->enqueue(event));
            result.trace = replay::hashEvent(result.trace, event);
            ++result.event_count;
        }
    };
    for (bool second : {false, true}) {
        const auto base = session->audioFrame() + 16 * 64;
        queue(replay::segment(second, 16, 1), base);
        // Include the block containing the final intra-block Program Change.
        const auto end = ((base + replay::kSegmentFrames) / 64 + 1) * 64;
        render(static_cast<unsigned>((end - session->audioFrame()) / 64));
        CHECK(session->audioProgram(0) == 16 && session->audioVoices(0) > 0);
        CHECK((session->audioLoops(0) > 0) == loop);
        const auto held = session->audioVoices(0);
        const auto replacement = second ? 16 : 1;
        unsigned rounds = 0;
        while (session->audioProgram(1) != replacement || session->phase(1) != Phase::Idle) {
            CHECK(++rounds <= 8);
            const auto status = session->controlService();
            CHECK(status == Service::Ok || status == Service::NoPending || status == Service::TransitionBusy);
            render(1);
            CHECK(session->audioProgram(0) == 16 && session->audioVoices(0) == held);
            CHECK((session->audioLoops(0) > 0) == loop);
        }
        CHECK(session->phase(0) == Phase::Idle);
        ++result.switches;
    }
    const auto cleanup = session->audioFrame() + 16 * 64;
    queue(replay::cleanup(), cleanup);
    const auto end = cleanup + replay::kTailFrames;
    render(static_cast<unsigned>((end - session->audioFrame()) / 64));
    CHECK(result.event_count == replay::kEvents && result.event_count == 117);
    CHECK(session->audioStats().events_applied == result.event_count && session->audioStats().events_rejected == 0);
    CHECK(session->audioVoices() == 0 && session->audioLoops() == 0);
    CHECK(result.channel_mask == replay::kChannelMask);
    CHECK(result.loop_channel_mask == (loop ? replay::kChannelMask : 0));
    CHECK(result.channel_voices[0] >= 3 && result.channel_voices[1] >= 1);
    const auto frames = session->audioFrame();
    const bool drained = session->audioDrainOutput(pcm.data());
    CHECK(drained == (mode == OutputMode::Peak64));
    if (drained) result.pcm.insert(result.pcm.end(), pcm.begin(), pcm.end());
    CHECK(!session->audioDrainOutput(pcm.data()));
    result.stats = session->audioOutputStats();
    CHECK(result.stats.input_frames == frames && result.stats.output_frames * 2 == result.pcm.size());
    CHECK(result.stats.output_frames == frames + result.stats.latency_frames);
    if (mode == OutputMode::Peak64) CHECK(result.stats.post_hard_clipped == 0 && result.stats.post_peak_abs <= 32767);
    CHECK(session->queueStats().rejected_full == 0 && session->queueStats().rejected_out_of_order == 0);
    session->requestStop(); session->audioStop(); session->controlStop();
    CHECK(session->publicationStats().stale_release_faults == 0);
    CHECK(session->stopQuiescent() && backing.releases == 1);
    CHECK(session->publicationStats().live_voice_refs == 0 && session->publicationStats().active_acquirers == 0);
    session.reset(); storage.reset();
    return result;
}

void musical_matrix(const char* path, const char* output_directory) {
    namespace replay = app::loop_realtime_replay;
    CHECK(replay::definitionHash(16, 1) == UINT64_C(4354443756388215394));
    std::ifstream input(path, std::ios::binary); CHECK(input.good());
    const std::vector<std::uint8_t> bank{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    CHECK(bank.size() == 5898214 && sha256(bank.data(), bank.size()) == kBankSha);
    std::ofstream manifest;
    if (output_directory) {
        manifest.open(std::filesystem::path(output_directory) / "musical-audition.json");
        CHECK(manifest.good());
        manifest << "{\n  \"schema\": \"v1-two-channel-musical-audition-1\",\n"
            << "  \"scene\": \"" << replay::kName << "\", \"definition_hash\": " << replay::definitionHash(16, 1) << ",\n"
            << "  \"bank_sha256\": \"" << kBankSha << "\",\n"
            << "  \"sample_rate_hz\": 44100, \"channels\": 2, \"bits_per_sample\": 16, \"events\": 117,\n"
            << "  \"configured_midi_channels\": 2, \"physical_timing_approved\": false, \"listening_approved\": false,\n"
            << "  \"notes\": \"Original two-channel phrase: chords, melody, sustain, independent controllers and Program Change while the other channel holds a note. Serialized Host control makes repeated PCM exact; target performs real concurrent preparation. Peak output adds 64 leading silent frames and explicitly drains its last block.\",\n"
            << "  \"renders\": [\n";
    }
    bool comma = false;
    for (bool loop : {false, true}) {
        OutputStats bypass_stats{};
        std::uint64_t bypass_trace = 0;
        std::vector<std::int16_t> bypass_pcm;
        for (auto mode : {OutputMode::Bypass, OutputMode::Peak64}) {
            const auto result = musical_replay(bank, loop, mode);
            const auto repeat = musical_replay(bank, loop, mode);
            CHECK(result.pcm == repeat.pcm && sameStats(result.stats, repeat.stats));
            CHECK(result.trace == repeat.trace && result.channel_voices == repeat.channel_voices &&
                  result.channel_loops == repeat.channel_loops && result.switches == repeat.switches);
            const auto& s = result.stats;
            if (mode == OutputMode::Bypass) {
                bypass_stats = s; bypass_trace = result.trace; bypass_pcm = result.pcm;
            }
            else {
                CHECK(s.raw_peak_abs == bypass_stats.raw_peak_abs && s.raw_overload_samples == bypass_stats.raw_overload_samples);
                CHECK(s.input_frames == bypass_stats.input_frames && result.trace == bypass_trace);
                CHECK(s.raw_overload_samples == 0 && s.gain_min_q30 == kUnity && s.limited_frames == 0);
                CHECK(result.pcm.size() == bypass_pcm.size() + 128);
                CHECK(std::all_of(result.pcm.begin(), result.pcm.begin() + 128, [](auto sample) { return sample == 0; }));
                CHECK(std::equal(bypass_pcm.begin(), bypass_pcm.end(), result.pcm.begin() + 128));
            }
            const char* loop_name = loop ? "on" : "off";
            const char* mode_name = mode == OutputMode::Peak64 ? "peak" : "bypass";
            const auto pcm_hash = checksum(result.pcm);
            std::printf("OUTPUT_REPLAY_RESULT loop=%s output=%s configured_channels=2 channel_mask=%u loop_channel_mask=%u events=%llu program_switches=%u voices=%u loops=%u channel0_voices=%u channel1_voices=%u channel0_loops=%u channel1_loops=%u definition_hash=%llu trace_hash=%llu raw_overload_samples=%llu raw_peak_abs=%u post_hard_clipped=%llu post_peak_abs=%u input_frames=%llu output_frames=%llu latency_frames=%u gain_min_q30=%u limited_frames=%llu checksum=%llu deterministic=1 physical_timing_approved=0 listening_approved=0\n",
                loop_name, mode_name, result.channel_mask, result.loop_channel_mask,
                static_cast<unsigned long long>(result.event_count), result.switches, result.voices, result.loops,
                result.channel_voices[0], result.channel_voices[1], result.channel_loops[0], result.channel_loops[1],
                static_cast<unsigned long long>(replay::definitionHash(16, 1)), static_cast<unsigned long long>(result.trace),
                static_cast<unsigned long long>(s.raw_overload_samples), unsigned(s.raw_peak_abs),
                static_cast<unsigned long long>(s.post_hard_clipped), unsigned(s.post_peak_abs),
                static_cast<unsigned long long>(s.input_frames), static_cast<unsigned long long>(s.output_frames),
                unsigned(s.latency_frames), unsigned(s.gain_min_q30), static_cast<unsigned long long>(s.limited_frames),
                static_cast<unsigned long long>(pcm_hash));
            if (output_directory) {
                const std::string filename = std::string("music-loop-") + loop_name + "-" + mode_name + ".wav";
                const auto wav_sha = write_wav(std::filesystem::path(output_directory) / filename, result.pcm);
                if (comma) manifest << ",\n";
                comma = true;
                manifest << "    {\"file\": \"" << filename << "\", \"sha256\": \"" << wav_sha
                    << "\", \"loop\": \"" << loop_name << "\", \"output\": \"" << mode_name
                    << "\", \"input_frames\": " << s.input_frames << ", \"output_frames\": " << s.output_frames
                    << ", \"latency_frames\": " << s.latency_frames << ", \"event_trace_hash\": " << result.trace
                    << ", \"sample_voices\": " << result.voices << ", \"loop_voices\": " << result.loops
                    << ", \"channel_mask\": " << result.channel_mask << ", \"loop_channel_mask\": " << result.loop_channel_mask
                    << ", \"raw_overload_samples\": " << s.raw_overload_samples << ", \"raw_peak_abs\": " << s.raw_peak_abs
                    << ", \"post_hard_clipped\": " << s.post_hard_clipped << ", \"post_peak_abs\": " << s.post_peak_abs
                    << ", \"gain_min_q30\": " << s.gain_min_q30 << ", \"limited_frames\": " << s.limited_frames
                    << ", \"pcm_checksum_fnv1a64\": " << pcm_hash << ", \"repeat_exact\": true}";
            }
        }
    }
    if (output_directory) { manifest << "\n  ]\n}\n"; manifest.close(); CHECK(manifest.good()); }
    std::puts("OUTPUT_HOST_CASE multichannel_musical_replay_deterministic PASS physical_timing_approved=0 listening_approved=0");
}

template<class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
struct ConcurrentCheckpoint {
    std::atomic<bool> armed{false}, entered{false}, released{false};
    static bool callback(void* opaque) {
        auto& self = *static_cast<ConcurrentCheckpoint*>(opaque);
        if (self.armed.load(std::memory_order_acquire)) {
            self.entered.store(true, std::memory_order_release);
            await([&] { return self.released.load(std::memory_order_acquire); });
        }
        return true;
    }
    void arm() {
        entered.store(false, std::memory_order_relaxed);
        released.store(false, std::memory_order_relaxed);
        armed.store(true, std::memory_order_release);
    }
};

void two_channel_concurrent_prepare_stop() {
    using MusicSession = app::LoopRealtimeSession<16, 8, 2>;
    using Phase = MusicSession::Phase;
    // The unmodified original UntilRelease fixture is intentional: this tests
    // actual sustain/NoteOff ownership, not just the musical phrase's coverage.
    const std::vector<std::uint8_t> bank{std::begin(loop_analysis_fixture::bytes), std::end(loop_analysis_fixture::bytes)};
    Backing backing;
    ConcurrentCheckpoint checkpoint;
    auto storage = std::make_unique<MusicSession::Storage>();
    auto session = std::make_unique<MusicSession>(*storage);
    app::StartupSerials serials;
    app::LoopRealtimeConfig config;
    config.analysis.experimental_opt_in = true;
    config.analysis.checkpoint = &ConcurrentCheckpoint::callback;
    config.analysis.checkpoint_context = &checkpoint;
    config.output_mode = OutputMode::Peak64;
    CHECK(session->start(backing.lease(bank), loop_analysis_fixture::digest, hash, serials, config) == app::LoopStartupStatus::Running);
    CHECK(session->beginActors());
    PcmBlock pcm{};
    auto push = [&](unsigned offset, Event type, unsigned channel, unsigned a, unsigned b = 0) {
        realtime::RealtimeEvent event{};
        event.timestamp_frame = session->audioFrame() + offset; event.type = type;
        event.channel = static_cast<std::uint8_t>(channel);
        event.data1 = static_cast<std::uint8_t>(a); event.data2 = static_cast<std::uint8_t>(b);
        CHECK(session->enqueue(event));
    };
    auto render = [&](unsigned blocks) {
        for (unsigned block = 0; block < blocks; ++block) CHECK(session->audioRender(pcm.data()));
    };
    push(0, Event::NoteOn, 0, 61, 100);
    push(0, Event::NoteOn, 1, 61, 100);
    push(8, Event::ControlChange, 0, 64, 127);
    push(16, Event::NoteOff, 0, 61);
    render(1);
    CHECK(session->audioVoices(0) == 1 && session->audioVoices(1) == 1);
    CHECK(session->audioLoops(0) == 1 && session->audioLoops(1) == 1);
    push(8, Event::ProgramChange, 0, 1);
    push(16, Event::ProgramChange, 1, 1);
    render(1);
    unsigned overlapped = 0;
    for (unsigned channel = 0; channel < 2; ++channel) {
        checkpoint.arm();
        Service prepared = Service::NoPending;
        std::thread control([&] {
            for (unsigned attempt = 0; attempt < 4 && session->phase(channel) != Phase::Ready; ++attempt)
                prepared = session->controlService();
        });
        await([&] { return checkpoint.entered.load(std::memory_order_acquire); });
        CHECK(session->phase(channel) == Phase::Preparing);
        CHECK(!session->stopQuiescent() && backing.releases == 0);
        if (channel == 0) {
            for (unsigned block = 0; block < 24; ++block) {
                render(1); ++overlapped;
                CHECK(session->audioProgram(0) == 0 && session->audioProgram(1) == 0);
                CHECK(session->audioLoops(0) == 1 && session->audioLoops(1) == 1);
            }
        } else {
            // Release sustain only on channel 0, then start a Direct Program 1
            // note there. Channel 1's held UntilRelease Voice must remain a
            // live loop while its own new Program is prepared concurrently.
            push(0, Event::ControlChange, 0, 64, 0);
            push(32, Event::NoteOn, 0, 60, 100);
            render(128); overlapped += 128;
            CHECK(session->audioProgram(0) == 1 && session->audioProgram(1) == 0);
            CHECK(session->audioVoices(0) == 1 && session->audioLoops(0) == 0);
            CHECK(session->audioVoices(1) == 1 && session->audioLoops(1) == 1);
        }
        checkpoint.released.store(true, std::memory_order_release);
        control.join();
        CHECK(prepared == Service::Ok && session->phase(channel) == Phase::Ready);
        render(1);
        CHECK(session->audioProgram(channel) == 1 && session->phase(channel) == Phase::Committed);
        // Channel 0's ACK occurs in the second iteration's controlService.
        // Channel 1 deliberately stays Committed without its retirement ACK.
    }
    CHECK(session->phase(0) == Phase::Idle && session->phase(1) == Phase::Committed);
    CHECK(session->audioVoices(1) == 1 && session->audioLoops(1) == 1);
    CHECK(session->audioOutputStats().post_hard_clipped == 0);
    CHECK(session->audioStats().events_rejected == 0);
    session->requestStop();
    CHECK(!session->stopQuiescent() && backing.releases == 0);
    session->controlStop();
    CHECK(!session->stopQuiescent() && backing.releases == 0);
    session->audioStop();
    CHECK(session->stopQuiescent() && backing.releases == 1);
    CHECK(session->publicationStats().live_voice_refs == 0 && session->publicationStats().active_acquirers == 0 &&
          session->publicationStats().stale_release_faults == 0);
    session.reset(); storage.reset();
    std::printf("OUTPUT_CONCURRENT_RESULT configured_channels=2 audio_blocks_during_prepare=%u stop_unacked=1 released=1 stale_refs=0 physical_timing_approved=0\n", overlapped);
    std::puts("OUTPUT_HOST_CASE two_channel_concurrent_prepare_stop PASS physical_timing_approved=0");
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    CHECK(argc == 2 || argc == 3);
    exact_bypass_and_master();
    delay_identity_and_drain();
    bounded_input_and_invalid_calls();
    stereo_linked_attack_and_release();
    real_bank_matrix(argv[1], argc == 3 ? argv[2] : nullptr);
    musical_matrix(argv[1], argc == 3 ? argv[2] : nullptr);
    two_channel_concurrent_prepare_stop();
    std::printf("OUTPUT_HOST_PASS checks=%u cases=7 physical_timing_approved=0 listening_approved=0\n", checks.load());
}
