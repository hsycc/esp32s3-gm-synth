// Test-only PCM16-LE harness, compatible with loop_cursor_probe CLI.
// Heap/file I/O live ONLY here. The real-bank Python verifier authenticates the
// source file; this isolated ownership harness uses a synthetic session identity.
#include "voice/experimental/PinnedLoopPlan.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gm_synth::voice::experimental;
namespace sample = gm_synth::sample;

std::uint64_t number(const char* text) {
    const std::string s(text);
    std::uint64_t n = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
        throw std::runtime_error("invalid integer");
    return n;
}
int main(int argc, char** argv) {
    try {
        if (argc != 8) throw std::runtime_error("usage: pinned_probe <pcm16le> <fade> <step> <total> <block> <off> <mode:1|3>");
        const auto fade = number(argv[2]), step = number(argv[3]), total = number(argv[4]);
        const auto block = number(argv[5]), off = number(argv[6]), mode = number(argv[7]);
        if ((fade && fade != 16 && fade != 32 && fade != 64) || !step || !total || total > 4000000 ||
            !block || block > 2048 || (mode != 1 && mode != 3)) throw std::runtime_error("bad arguments");
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("cannot read PCM");
        const auto bytes = f.tellg();
        if (bytes < 8 || bytes > 2000000 || bytes % 2) throw std::runtime_error("bad PCM size");
        std::vector<char> raw(static_cast<std::size_t>(bytes));
        f.seekg(0); f.read(raw.data(), bytes);
        if (!f) throw std::runtime_error("truncated PCM");
        std::vector<std::int16_t> pcm(raw.size() / 2);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            const auto u = static_cast<unsigned char>(raw[2*i]) |
                (static_cast<unsigned>(static_cast<unsigned char>(raw[2*i+1])) << 8);
            pcm[i] = static_cast<std::int16_t>(u < 32768 ? static_cast<int>(u) : static_cast<int>(u) - 65536);
        }
        sample::PublishedSampleDirectory directory;
        sample::ResidentRecord record;
        const auto n = static_cast<std::uint32_t>(pcm.size());
        record.bank_generation = directory.bankGeneration(); record.sample_generation = 1;
        record.pcm = pcm.data(); record.descriptor.id = 17;
        record.descriptor.frames = n; record.descriptor.pcm_bytes = n * 2;
        record.descriptor.sample_rate = 44100; record.descriptor.loop_end = n;
        if (!directory.publish(17, &record)) throw std::runtime_error("publish failed");
        LoopPlanRequest request;
        request.experimental_opt_in = true;
        request.context.asset_sha256.fill(0x1f); // synthetic, not a claimed asset hash
        request.context.session_serial = 1; request.context.bank_generation = record.bank_generation;
        request.context.sample_generation = 1; request.context.sample_id = 17;
        request.context.sample_frames = n; request.context.sample_rate = 44100;
        request.context.region = {0, n, 0, n};
        request.context.mode = mode == 1 ? CursorLoopMode::Continuous : CursorLoopMode::UntilRelease;
        request.context.noteon_addresses_fixed = true;
        request.fade = {fade != 0, static_cast<std::uint16_t>(fade)};
        PinnedLoopPlan plan;
        PinnedLoopPlayback cursor;
        if (plan.bind(directory, request, request.context) != LoopBindingStatus::Ok ||
            cursor.start(directory, plan, request.context, step) != LoopBindingStatus::Ok)
            throw std::runtime_error("plan/start rejected");
        // Retire the plan and publication BEFORE comparison. Only the playback
        // pin now keeps the exact record alive, across every wrap and NoteOff.
        if (directory.unpublish(17) != &record || !plan.reset() || directory.canReclaim(&record))
            throw std::runtime_error("retirement did not retain playback pin");
        std::vector<std::int16_t> buffer(block);
        std::vector<char> encoded(block * 2);
        std::uint64_t done = 0;
        while (done < total && cursor.active()) {
            if (done == off) cursor.noteOff();
            auto count = std::min(block, total - done);
            if (done < off) count = std::min(count, off - done);
            const auto got = cursor.render(buffer.data(), count);
            if (!got) throw std::runtime_error("no progress");
            for (std::size_t i = 0; i < got; ++i) {
                const auto u = static_cast<std::uint16_t>(buffer[i]);
                encoded[2*i] = static_cast<char>(u & 255); encoded[2*i+1] = static_cast<char>(u >> 8);
            }
            std::cout.write(encoded.data(), static_cast<std::streamsize>(got * 2));
            done += got;
        }
        if (!cursor.reset() || !directory.canReclaim(&record) || directory.stats().live_voice_refs ||
            directory.stats().stale_release_faults) throw std::runtime_error("pin leaked or double released");
        if (!std::cout) throw std::runtime_error("output failed");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
