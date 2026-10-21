#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2ResolvedZone.h"
#include "storage/RandomAccessReader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::NativeZoneFlagInvalidLoopRange;
using gm_synth::sf2::ResolveProgramResult;
using gm_synth::sf2::ResolveProgramStatus;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2ResolvedZoneResolver;
using gm_synth::storage::RandomAccessReader;

class FileReader final : public RandomAccessReader {
public:
    explicit FileReader(const char* path) : file_(path, std::ios::binary) {
        if (!file_) {
            return;
        }
        file_.seekg(0, std::ios::end);
        const std::streamoff end = file_.tellg();
        if (end >= 0) {
            size_ = static_cast<std::uint64_t>(end);
        }
        file_.clear();
    }

    bool valid() const { return file_.is_open() && size_ != 0; }
    std::uint64_t size() const override { return size_; }

    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        if (!valid() || destination == nullptr || offset > size_ || bytes > size_ - offset) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) {
            return false;
        }
        file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
        return file_.gcount() == static_cast<std::streamsize>(bytes);
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

// Host-only PCM16 seam probe; a discontinuity is a listening-review signal.
bool pcmPoint(RandomAccessReader& reader, const Sf2PdtaReader& pdta,
              std::int64_t index, std::int32_t* value) {
    if (value == nullptr || index < 0 ||
        static_cast<std::uint64_t>(index) >= pdta.smplPointCount()) {
        return false;
    }
    std::uint8_t bytes[2]{};
    if (!reader.readAt(pdta.smplPayloadOffset() +
            static_cast<std::uint64_t>(index) * 2u, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8u));
    return true;
}

std::int32_t generatorValue(const NativeResolvedZone& zone, std::uint16_t oper) {
    return zone.generators.value(oper, 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_sf2_loop_inventory <sf2> <asset_id>\n";
        return 2;
    }

    FileReader reader(argv[1]);
    const std::string asset = argv[2];
    if (!reader.valid()) {
        std::cerr << "M7_LOOP_FATAL asset=" << asset << " reason=asset_not_readable\n";
        return 2;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_LOOP_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 2;
    }

    Sf2ResolvedZoneResolver resolver;
    std::size_t invalid_programs = 0;
    std::size_t invalid_zones = 0;
    std::size_t start_after_loop = 0;
    std::size_t empty_loop = 0;
    std::size_t loop_after_end = 0;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset) || preset.program > 255u) {
            continue;
        }

        ResolveProgramResult count{};
        const ResolveProgramStatus count_status = resolver.resolveProgram(
            pdta,
            preset.bank,
            static_cast<std::uint16_t>(preset.program),
            nullptr,
            0,
            &count);
        if (count_status != ResolveProgramStatus::Ok &&
            count_status != ResolveProgramStatus::OutputTooSmall) {
            continue;
        }
        const std::size_t required = count_status == ResolveProgramStatus::OutputTooSmall
            ? count.required_capacity
            : count.zone_count;
        if (required == 0) {
            continue;
        }

        std::vector<NativeResolvedZone> zones(required);
        ResolveProgramResult resolved{};
        if (resolver.resolveProgram(
                pdta,
                preset.bank,
                static_cast<std::uint16_t>(preset.program),
                zones.data(),
                zones.size(),
                &resolved) != ResolveProgramStatus::Ok) {
            continue;
        }

        bool program_invalid = false;
        for (std::size_t zone_index = 0; zone_index < resolved.zone_count; ++zone_index) {
            const NativeResolvedZone& zone = zones[zone_index];
            if ((zone.flags & NativeZoneFlagInvalidLoopRange) == 0) {
                continue;
            }
            program_invalid = true;
            ++invalid_zones;

            const bool bad_start = zone.sample_start > zone.loop_start;
            const bool bad_order = zone.loop_start >= zone.loop_end;
            const bool bad_end = zone.loop_end > zone.sample_end;
            start_after_loop += bad_start ? 1u : 0u;
            empty_loop += bad_order ? 1u : 0u;
            loop_after_end += bad_end ? 1u : 0u;

            std::cout << "M7_LOOP_DETAIL"
                      << " asset=" << asset
                      << " bank=" << preset.bank
                      << " program=" << preset.program
                      << " zone=" << zone_index
                      << " sample_id=" << zone.sample_id
                      << " sample_modes=" << zone.sampleModes()
                      << " shdr_start=" << zone.shdr_start
                      << " shdr_end=" << zone.shdr_end
                      << " shdr_loop_start=" << zone.shdr_loop_start
                      << " shdr_loop_end=" << zone.shdr_loop_end
                      << " sample_start=" << zone.sample_start
                      << " sample_end=" << zone.sample_end
                      << " loop_start=" << zone.loop_start
                      << " loop_end=" << zone.loop_end
                      << " gen0=" << generatorValue(zone, 0)
                      << " gen1=" << generatorValue(zone, 1)
                      << " gen2=" << generatorValue(zone, 2)
                      << " gen3=" << generatorValue(zone, 3)
                      << " gen4=" << generatorValue(zone, 4)
                      << " gen12=" << generatorValue(zone, 12)
                      << " gen45=" << generatorValue(zone, 45)
                      << " gen50=" << generatorValue(zone, 50)
                      << " bad_start=" << (bad_start ? 1 : 0)
                      << " bad_order=" << (bad_order ? 1 : 0)
                      << " bad_end=" << (bad_end ? 1 : 0)
                      << " normalized=" << (normalized ? 1 : 0)
                      << " seam_pcm16_jump=" << seam_jump
                      << " terminal_pcm16_step=" << terminal_step
                      << '\n';
        }
        if (program_invalid) {
            ++invalid_programs;
        }
    }

    std::cout << "M7_LOOP_SUMMARY"
              << " asset=" << asset
              << " invalid_programs=" << invalid_programs
              << " invalid_zones=" << invalid_zones
              << " start_after_loop=" << start_after_loop
              << " empty_loop=" << empty_loop
              << " loop_after_end=" << loop_after_end
              << '\n';
    return 0;
}
