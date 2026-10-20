#include "sf2/Sf2PreparedNoteOnRouteResolver.h"
#include "sf2/Sf2ResolvedZone.h"
#include "storage/RandomAccessReader.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gm_synth::sf2::PreparedNoteOnRouteResolveResult;
using gm_synth::sf2::PreparedNoteOnRouteResolveStatus;
using gm_synth::sf2::ResolveProgramResult;
using gm_synth::sf2::ResolveProgramStatus;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PreparedNoteOnRouteBundle;
using gm_synth::sf2::Sf2PreparedNoteOnRouteResolver;
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
        std::cerr << "usage: test_sf2_prepared_note_on_route_inventory <sf2> <asset_id>\n";
        return 2;
    }

    FileReader reader(argv[1]);
    const std::string asset = argv[2];
    if (!reader.valid()) {
        std::cerr << "M7_PREPARED_ROUTE_FATAL asset=" << asset
                  << " reason=asset_not_readable\n";
        return 2;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_PREPARED_ROUTE_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError())
                  << '\n';
        return 2;
    }

    Sf2PreparedNoteOnRouteResolver route_resolver;
    Sf2ResolvedZoneResolver zone_resolver;
    std::size_t programs = 0;
    std::size_t route_programs = 0;
    std::size_t route_zones = 0;
    std::size_t dual_filter_q_zones = 0;
    std::size_t alignment_failures = 0;
    std::uint32_t overflow_records = 0;
    std::uint16_t max_routes_per_zone = 0;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        if (!pdta.presetHeader(preset_index, &preset)) {
            return 2;
        }

        ResolveProgramResult native_query{};
        const ResolveProgramStatus native_status = zone_resolver.resolveProgram(
            pdta,
            preset.bank,
            preset.program,
            nullptr,
            0,
            &native_query);
        if (native_status != ResolveProgramStatus::Ok &&
            native_status != ResolveProgramStatus::OutputTooSmall) {
            std::cerr << "M7_PREPARED_ROUTE_FATAL asset=" << asset
                      << " reason=native_resolve bank=" << preset.bank
                      << " program=" << preset.program << '\n';
            return 2;
        }

        PreparedNoteOnRouteResolveResult route_query{};
        const PreparedNoteOnRouteResolveStatus route_status = route_resolver.resolveProgram(
            pdta,
            preset.bank,
            preset.program,
            nullptr,
            0,
            &route_query);
        if (route_status != PreparedNoteOnRouteResolveStatus::Ok &&
            route_status != PreparedNoteOnRouteResolveStatus::OutputTooSmall) {
            std::cerr << "M7_PREPARED_ROUTE_FATAL asset=" << asset
                      << " reason=route_resolve bank=" << preset.bank
                      << " program=" << preset.program << '\n';
            return 2;
        }

        if (native_query.required_capacity != route_query.required_capacity) {
            ++alignment_failures;
            continue;
        }

        std::vector<Sf2PreparedNoteOnRouteBundle> bundles(route_query.required_capacity);
        PreparedNoteOnRouteResolveResult resolved{};
        const PreparedNoteOnRouteResolveStatus resolved_status = route_resolver.resolveProgram(
            pdta,
            preset.bank,
            preset.program,
            bundles.empty() ? nullptr : bundles.data(),
            bundles.size(),
            &resolved);
        if (resolved_status != PreparedNoteOnRouteResolveStatus::Ok ||
            resolved.zone_count != native_query.required_capacity) {
            ++alignment_failures;
            continue;
        }

        ++programs;
        bool program_has_route = false;
        overflow_records += resolved.bundle_overflow_records;
        if (resolved.max_routes_per_zone > max_routes_per_zone) {
            max_routes_per_zone = resolved.max_routes_per_zone;
        }
        for (const auto& bundle : bundles) {
            if (bundle.count == 0) {
                continue;
            }
            program_has_route = true;
            ++route_zones;
            for (std::size_t index = 0; index < bundle.count; ++index) {
                const auto& route = bundle.routes[index];
                if (route.source_oper == 0x0402u &&
                    route.destination_oper == 9u &&
                    route.amount_source_oper == 0x0D02u) {
                    ++dual_filter_q_zones;
                }
            }
        }
        if (program_has_route) {
            ++route_programs;
        }
    }

    std::cout << "M7_PREPARED_ROUTE_SUMMARY"
              << " asset=" << asset
              << " programs=" << programs
              << " route_programs=" << route_programs
              << " route_zones=" << route_zones
              << " max_routes_per_zone=" << max_routes_per_zone
              << " bundle_overflow_records=" << overflow_records
              << " dual_filter_q_zones=" << dual_filter_q_zones
              << " zone_alignment_failures=" << alignment_failures
              << '\n';

    if (alignment_failures != 0 || overflow_records != 0) {
        return 1;
    }
    return 0;
}
