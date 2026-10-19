#include "sf2/Sf2PdtaReader.h"
#include "storage/RandomAccessReader.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_sf2_id_space <sf2> <asset_id>\n";
        return 2;
    }

    const char* path = argv[1];
    const std::string asset = argv[2];
    FileReader reader(path);
    if (!reader.valid()) {
        std::cerr << "M7_SF2_ID_SPACE_FATAL asset=" << asset
                  << " reason=asset_not_readable path=" << path << '\n';
        return 1;
    }

    Sf2PdtaReader pdta;
    if (!pdta.begin(reader)) {
        std::cerr << "M7_SF2_ID_SPACE_FATAL asset=" << asset
                  << " reason=pdta_open error=" << static_cast<unsigned>(pdta.lastError())
                  << '\n';
        return 1;
    }

    const std::size_t samples = pdta.sampleCount();
    std::cout << "M7_SF2_ID_SPACE"
              << " asset=" << asset
              << " file_bytes=" << reader.size()
              << " presets=" << pdta.presetCount()
              << " samples=" << samples
              << " max_sample_id=" << (samples == 0 ? 0 : samples - 1u)
              << " current_resident_capacity=512"
              << '\n';
    return 0;
}
