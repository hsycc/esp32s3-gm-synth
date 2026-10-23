#include "sf2/Sf2PdtaReader.h"
#include "storage/RandomAccessReader.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <tuple>

namespace {

using gm_synth::sf2::Sf2PdtaReader;
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

struct RouteKey {
    std::uint16_t source = 0;
    std::uint16_t destination = 0;
    std::int16_t amount = 0;
    std::uint16_t amount_source = 0;
    std::uint16_t transform = 0;

    bool operator<(const RouteKey& other) const {
        return std::tie(source, destination, amount, amount_source, transform) <
            std::tie(
                other.source,
                other.destination,
                other.amount,
                other.amount_source,
                other.transform);
    }
};

bool terminal(const Sf2PdtaReader::Modulator& mod) {
    return mod.source_oper == 0 && mod.destination_oper == 0 &&
        mod.amount == 0 && mod.amount_source_oper == 0 && mod.transform_oper == 0;
}

void addRoute(
    const Sf2PdtaReader::Modulator& mod,
    std::map<RouteKey, std::size_t>* routes) {
    if (routes == nullptr || terminal(mod)) {
        return;
    }
    ++(*routes)[RouteKey{
        mod.source_oper,
        mod.destination_oper,
        mod.amount,
        mod.amount_source_oper,
        mod.transform_oper,
    }];
}

void printRoutes(
    const std::string& asset,
    const char* scope,
    const std::map<RouteKey, std::size_t>& routes) {
    for (const auto& entry : routes) {
        const RouteKey& route = entry.first;
        std::cout << "M7_MOD_ROUTE"
                  << " asset=" << asset
                  << " scope=" << scope
                  << " source=0x" << std::hex << std::setw(4) << std::setfill('0')
                  << route.source
                  << " destination=" << std::dec << route.destination
                  << " amount=" << route.amount
                  << " amount_source=0x" << std::hex << std::setw(4)
                  << route.amount_source
                  << " transform=" << std::dec << route.transform
                  << " count=" << entry.second
                  << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_sf2_modulator_inventory <sf2> <asset_id>\n";
        return 2;
    }

    FileReader reader(argv[1]);
    const std::string asset = argv[2];
    if (!reader.valid()) {
        std::cerr << "M7_MOD_FATAL asset=" << asset << " reason=asset_not_readable\n";
        return 2;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_MOD_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError()) << '\n';
        return 2;
    }

    std::map<RouteKey, std::size_t> preset_routes;
    std::map<RouteKey, std::size_t> instrument_routes;
    std::size_t preset_records = 0;
    std::size_t instrument_records = 0;

    for (std::size_t index = 0; index < pdta.presetModulatorRecordCount(); ++index) {
        Sf2PdtaReader::Modulator mod{};
        if (!pdta.presetModulator(index, &mod)) {
            return 2;
        }
        if (!terminal(mod)) {
            ++preset_records;
            addRoute(mod, &preset_routes);
        }
    }
    for (std::size_t index = 0; index < pdta.instrumentModulatorRecordCount(); ++index) {
        Sf2PdtaReader::Modulator mod{};
        if (!pdta.instrumentModulator(index, &mod)) {
            return 2;
        }
        if (!terminal(mod)) {
            ++instrument_records;
            addRoute(mod, &instrument_routes);
        }
    }

    printRoutes(asset, "preset", preset_routes);
    printRoutes(asset, "instrument", instrument_routes);
    std::cout << "M7_MOD_SUMMARY"
              << " asset=" << asset
              << " preset_records=" << preset_records
              << " preset_unique_routes=" << preset_routes.size()
              << " instrument_records=" << instrument_records
              << " instrument_unique_routes=" << instrument_routes.size()
              << '\n';
    return 0;
}
