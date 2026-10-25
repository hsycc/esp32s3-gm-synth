// Host-only exporter: reuse production Prepare instead of reimplementing SF2
// hierarchy/offset/sampleMode rules in Python. No PCM or runtime state is edited.
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "storage/RandomAccessReader.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
class FileReader final : public gm_synth::storage::RandomAccessReader {
public:
    explicit FileReader(const char* path) : f_(path, std::ios::binary) {
        if (!f_) return;
        f_.seekg(0, std::ios::end);
        const auto end = f_.tellg();
        if (end >= 0) bytes_ = static_cast<std::uint64_t>(end);
        f_.clear();
    }
    std::uint64_t size() const override { return bytes_; }
    bool readAt(std::uint64_t pos, void* out, std::size_t n) override {
        if (!f_.is_open() || !out || pos > bytes_ || n > bytes_ - pos) return false;
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(pos));
        if (!f_) return false;
        f_.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
        return f_.gcount() == static_cast<std::streamsize>(n);
    }
private:
    std::ifstream f_;
    std::uint64_t bytes_ = 0;
};
// Encode bytes with non-ASCII as \u00XX: preserves SF2's byte names in JSON.
std::string quoted(const char* s) {
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (; *s; ++s) {
        const auto c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}
}

int main(int argc, char** argv) {
    using namespace gm_synth;
    if (argc != 2) { std::cerr << "usage: export_prepared_loops <sf2>\n"; return 2; }
    FileReader reader(argv[1]);
    sf2::Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "pdta_open_failed error=" << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 2;
    }
    constexpr auto cap = gm::PlaybackProgramSnapshot::kMaxZones;
    constexpr auto keys = gm::PlaybackProgramSnapshot::kMidiKeyCount;
    std::vector<sf2::NativeResolvedZone> native(cap);
    std::vector<sf2::ResolvedZoneModulation> mods(cap);
    std::vector<gm::PlaybackZone> zones(cap);
    std::array<gm::PlaybackProgramKeySpan, keys> spans{};
    std::vector<std::uint16_t> indices(cap * keys);
    std::vector<sample::SampleId> dependencies(cap);
    sf2::Sf2PlaybackProgramPreparer preparer;
    std::set<std::pair<std::uint16_t, std::uint16_t>> seen;
    std::size_t ok = 0, rejected = 0, emitted = 0;
    std::cout << "{\"kind\":\"meta\",\"schema\":1,\"producer\":\"Sf2PlaybackProgramPreparer\","
              << "\"file_bytes\":" << reader.size() << ",\"smpl_offset\":" << pdta.smplPayloadOffset()
              << ",\"smpl_points\":" << pdta.smplPointCount() << ",\"presets\":" << pdta.presetCount() << "}\n";
    for (std::size_t p = 0; p < pdta.presetCount(); ++p) {
        sf2::Sf2PdtaReader::PresetHeader h{};
        if (!pdta.presetHeader(p, &h)) return 2;
        gm::PlaybackProgramSnapshot snapshot{};
        sf2::PlaybackPrepareRequest req{};
        req.channel = h.bank == 128 ? 9 : 0;
        req.bank = h.bank; req.program = static_cast<std::uint8_t>(h.program);
        req.token = static_cast<std::uint32_t>(p + 1); req.bank_generation = 1;
        req.native_scratch = native.data(); req.native_capacity = native.size();
        req.modulation_scratch = mods.data(); req.modulation_capacity = mods.size();
        req.playback_zones = zones.data(); req.playback_capacity = zones.size();
        req.key_spans = spans.data(); req.key_zone_indices = indices.data();
        req.key_zone_index_capacity = indices.size();
        req.dependencies = dependencies.data(); req.dependency_capacity = dependencies.size();
        req.snapshot = &snapshot; req.policy = sf2::PlaybackPreparePolicy::StrictQualified;
        const bool identity_ok = h.program <= 255 && seen.emplace(h.bank, h.program).second;
        const auto result = identity_ok ? preparer.prepare(pdta, req) : sf2::PlaybackPrepareResult{};
        if (!identity_ok || !result.ok() || !snapshot.valid()) {
            ++rejected;
            std::cout << "{\"kind\":\"program_rejected\",\"bank\":" << h.bank
                      << ",\"program\":" << h.program << ",\"prepare_status\":"
                      << static_cast<unsigned>(result.status) << ",\"issues\":" << result.issue_union << "}\n";
            continue;
        }
        ++ok;
        for (std::size_t i = 0; i < snapshot.zone_count; ++i) {
            if (emitted >= 190000) { std::cerr << "export_record_limit\n"; return 2; }
            const auto& z = snapshot.zones[i];
            sf2::Sf2PdtaReader::SampleHeader sh{};
            if (!pdta.sampleHeader(z.sample_id, &sh)) return 2;
            const auto& region = z.sample_region;
            const std::uint64_t start = region.enabled ? sh.start + std::uint64_t{region.start_frame} : sh.start;
            const std::uint64_t end = region.enabled ? sh.start + std::uint64_t{region.end_frame} : sh.end;
            const bool looping = z.loop_mode != voice::LoopMode::None;
            const std::uint64_t ls = !looping ? 0 : region.enabled ? sh.start + std::uint64_t{region.loop_start_frame} : sh.start_loop;
            const std::uint64_t le = !looping ? 0 : region.enabled ? sh.start + std::uint64_t{region.loop_end_frame} : sh.end_loop;
            bool dynamic = false;
            for (std::size_t m = 0; m < z.prepared_note_on_modulators.count; ++m) {
                const auto dest = z.prepared_note_on_modulators.routes[m].destination_oper;
                if (dest == 0 || dest == 4) dynamic = true;
            }
            const char* mode = !looping ? "none" : z.loop_mode == voice::LoopMode::Continuous ? "continuous" : "until_release";
            std::cout << "{\"kind\":\"zone\",\"scope\":\"prepared_zone\",\"bank\":" << h.bank
                << ",\"program\":" << h.program << ",\"preset_index\":" << p << ",\"zone_index\":" << i
                << ",\"preset_name\":" << quoted(h.name.data()) << ",\"sample_id\":" << z.sample_id
                << ",\"name\":" << quoted(sh.name.data()) << ",\"rate\":" << sh.sample_rate
                << ",\"sample_type\":" << sh.sample_type << ",\"sample_link\":" << sh.sample_link
                << ",\"source_start\":" << sh.start << ",\"source_end\":" << sh.end
                << ",\"start\":" << start << ",\"end\":" << end << ",\"loop_start\":" << ls << ",\"loop_end\":" << le
                << ",\"raw_loop_start\":" << sh.start_loop << ",\"raw_loop_end\":" << sh.end_loop
                << ",\"loop_mode\":\"" << mode << "\",\"region_override\":" << (region.enabled ? "true" : "false")
                << ",\"noteon_address_modulated\":" << (dynamic ? "true" : "false")
                << ",\"key_low\":" << static_cast<unsigned>(z.key_low) << ",\"key_high\":" << static_cast<unsigned>(z.key_high)
                << ",\"velocity_low\":" << static_cast<unsigned>(z.velocity_low) << ",\"velocity_high\":" << static_cast<unsigned>(z.velocity_high)
                << "}\n";
            ++emitted;
        }
    }
    std::cout << "{\"kind\":\"summary\",\"prepared_programs\":" << ok << ",\"rejected_programs\":"
              << rejected << ",\"zones\":" << emitted << "}\n";
    return std::cout.good() ? 0 : 2;
}
