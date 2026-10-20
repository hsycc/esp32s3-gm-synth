#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2ResolvedZone.h"
#include "storage/RandomAccessReader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gm_synth::sf2::NativeResolvedZone;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_sf2_zone_shape_inventory <sf2> <asset_id>\n";
        return 2;
    }

    FileReader reader(argv[1]);
    const std::string asset = argv[2];
    if (!reader.valid()) {
        return 2;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        return 2;
    }

    Sf2ResolvedZoneResolver resolver;
    std::size_t large_programs = 0;
    std::size_t bank_max_zones = 0;
    std::size_t bank_max_key_candidates = 0;
    std::size_t bank_max_key_velocity_matches = 0;
    std::size_t bank_max_key_refs = 0;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset) || preset.program > 255u) {
            continue;
        }

        ResolveProgramResult sizing{};
        const ResolveProgramStatus sizing_status = resolver.resolveProgram(
            pdta,
            preset.bank,
            preset.program,
            nullptr,
            0,
            &sizing);
        if (sizing_status != ResolveProgramStatus::OutputTooSmall ||
            sizing.required_capacity == 0) {
            continue;
        }

        std::vector<NativeResolvedZone> zones(sizing.required_capacity);
        ResolveProgramResult resolved{};
        if (resolver.resolveProgram(
                pdta,
                preset.bank,
                preset.program,
                zones.data(),
                zones.size(),
                &resolved) != ResolveProgramStatus::Ok) {
            return 2;
        }

        std::size_t max_key_candidates = 0;
        std::size_t max_key_velocity_matches = 0;
        std::size_t key_refs = 0;
        for (const NativeResolvedZone& zone : zones) {
            key_refs += static_cast<std::size_t>(zone.key_high - zone.key_low) + 1u;
        }
        for (unsigned key = 0; key <= 127; ++key) {
            std::size_t key_candidates = 0;
            for (const NativeResolvedZone& zone : zones) {
                if (key >= zone.key_low && key <= zone.key_high) {
                    ++key_candidates;
                }
            }
            max_key_candidates = std::max(max_key_candidates, key_candidates);

            for (unsigned velocity = 0; velocity <= 127; ++velocity) {
                std::size_t matches = 0;
                for (const NativeResolvedZone& zone : zones) {
                    if (key >= zone.key_low && key <= zone.key_high &&
                        velocity >= zone.velocity_low && velocity <= zone.velocity_high) {
                        ++matches;
                    }
                }
                max_key_velocity_matches = std::max(max_key_velocity_matches, matches);
            }
        }

        bank_max_zones = std::max(bank_max_zones, zones.size());
        bank_max_key_candidates = std::max(bank_max_key_candidates, max_key_candidates);
        bank_max_key_velocity_matches = std::max(
            bank_max_key_velocity_matches,
            max_key_velocity_matches);
        bank_max_key_refs = std::max(bank_max_key_refs, key_refs);

        if (zones.size() > 256u) {
            ++large_programs;
            std::cout << "M7_ZONE_SHAPE"
                      << " asset=" << asset
                      << " bank=" << preset.bank
                      << " program=" << preset.program
                      << " zones=" << zones.size()
                      << " max_key_candidates=" << max_key_candidates
                      << " max_key_velocity_matches=" << max_key_velocity_matches
                      << " key_refs=" << key_refs
                      << " key_index_bytes_u16=" << (key_refs * sizeof(std::uint16_t) + 129u * sizeof(std::uint16_t))
                      << '\n';
        }
    }

    std::cout << "M7_ZONE_SHAPE_SUMMARY"
              << " asset=" << asset
              << " large_programs=" << large_programs
              << " max_zones=" << bank_max_zones
              << " max_key_candidates=" << bank_max_key_candidates
              << " max_key_velocity_matches=" << bank_max_key_velocity_matches
              << " max_key_refs=" << bank_max_key_refs
              << '\n';
    return 0;
}
