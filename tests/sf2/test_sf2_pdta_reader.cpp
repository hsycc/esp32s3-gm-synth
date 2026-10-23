#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::storage::RandomAccessReader;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

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

void test_real_bank_pdta_counts() {
    struct Case {
        const char* path;
        std::size_t presets;
        std::size_t pbag;
        std::size_t pmod;
        std::size_t pgen;
        std::size_t instruments;
        std::size_t ibag;
        std::size_t imod;
        std::size_t igen;
        std::size_t samples;
    };

    const std::array<Case, 3> cases{{
        {
            "assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2",
            67, 68, 1, 68, 67, 68, 1, 403, 67,
        },
        {
            "assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2",
            35, 279, 71, 1842, 44, 367, 231, 1590, 186,
        },
        {
            "assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2",
            23, 93, 1, 300, 25, 216, 39, 1856, 102,
        },
    }};

    for (const Case& test : cases) {
        FileReader reader(test.path);
        CHECK(reader.valid());

        Sf2PdtaReader pdta;
        CHECK(pdta.begin(reader));
        CHECK(pdta.lastError() == Sf2PdtaReader::Error::None);
        CHECK(pdta.presetCount() == test.presets);
        CHECK(pdta.presetHeaderRecordCount() == test.presets + 1u);
        CHECK(pdta.presetBagRecordCount() == test.pbag);
        CHECK(pdta.presetModulatorRecordCount() == test.pmod);
        CHECK(pdta.presetGeneratorRecordCount() == test.pgen);
        CHECK(pdta.instrumentCount() == test.instruments);
        CHECK(pdta.instrumentHeaderRecordCount() == test.instruments + 1u);
        CHECK(pdta.instrumentBagRecordCount() == test.ibag);
        CHECK(pdta.instrumentModulatorRecordCount() == test.imod);
        CHECK(pdta.instrumentGeneratorRecordCount() == test.igen);
        CHECK(pdta.sampleCount() == test.samples);
        CHECK(pdta.sampleHeaderRecordCount() == test.samples + 1u);
        CHECK(pdta.smplPayloadOffset() != 0);
        CHECK(pdta.smplPayloadBytes() != 0);
        CHECK(pdta.smplPointCount() * 2u == pdta.smplPayloadBytes());
    }
}

void test_fm_records_match_reference_baseline() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());

    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    Sf2PdtaReader::PresetHeader preset{};
    CHECK(pdta.presetHeader(0, &preset));
    CHECK(preset.program == 0);
    CHECK(preset.bank == 0);
    CHECK(preset.bag_index == 0);
    CHECK(std::strncmp(preset.name.data(), "Acoustic Grand Pian", 19) == 0);

    Sf2PdtaReader::Bag preset_bag{};
    CHECK(pdta.presetBag(0, &preset_bag));
    CHECK(preset_bag.generator_index == 0);

    Sf2PdtaReader::Generator preset_gen{};
    CHECK(pdta.presetGenerator(preset_bag.generator_index, &preset_gen));
    CHECK(preset_gen.oper == 41);  // instrument
    CHECK(preset_gen.amount == 0);

    Sf2PdtaReader::InstrumentHeader instrument{};
    CHECK(pdta.instrumentHeader(0, &instrument));
    CHECK(instrument.bag_index == 0);

    Sf2PdtaReader::SampleHeader sample{};
    CHECK(pdta.sampleHeader(0, &sample));
    CHECK(sample.end > sample.start);
    CHECK(sample.end - sample.start == 63945u);
    CHECK(sample.sample_rate == 44100u);
    CHECK(sample.original_pitch == 60u);
    CHECK(sample.pitch_correction == 0);
    CHECK(!sample.isRom());
}

void test_generator_range_and_signed_amount_helpers() {
    Sf2PdtaReader::Generator range{};
    range.amount = static_cast<std::uint16_t>((100u << 8u) | 20u);
    CHECK(range.rangeLow() == 20u);
    CHECK(range.rangeHigh() == 100u);

    Sf2PdtaReader::Generator negative{};
    negative.amount = 0xff9cu;  // -100 in two's-complement
    CHECK(negative.signedAmount() == -100);
}

void test_out_of_range_record_access_is_rejected() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());

    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    Sf2PdtaReader::PresetHeader preset{};
    CHECK(!pdta.presetHeader(pdta.presetHeaderRecordCount(), &preset));
    CHECK(pdta.lastError() == Sf2PdtaReader::Error::InvalidIndexRange);

    Sf2PdtaReader::SampleHeader sample{};
    CHECK(!pdta.sampleHeader(pdta.sampleHeaderRecordCount(), &sample));
    CHECK(pdta.lastError() == Sf2PdtaReader::Error::InvalidIndexRange);
}

void test_invalid_riff_is_rejected() {
    class MemoryReader final : public RandomAccessReader {
    public:
        std::uint64_t size() const override { return bytes.size(); }
        bool readAt(std::uint64_t offset, void* destination, std::size_t count) override {
            if (destination == nullptr || offset > bytes.size() || count > bytes.size() - offset) {
                return false;
            }
            std::memcpy(destination, bytes.data() + offset, count);
            return true;
        }

        std::array<std::uint8_t, 12> bytes{{
            'N', 'O', 'P', 'E',
            4, 0, 0, 0,
            's', 'f', 'b', 'k',
        }};
    } reader;

    Sf2PdtaReader pdta;
    CHECK(!pdta.begin(reader));
    CHECK(pdta.lastError() == Sf2PdtaReader::Error::InvalidRiff);
}

}  // namespace

int main() {
    test_real_bank_pdta_counts();
    test_fm_records_match_reference_baseline();
    test_generator_range_and_signed_amount_helpers();
    test_out_of_range_record_access_is_rejected();
    test_invalid_riff_is_rejected();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 pdta-reader checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 pdta-reader host checks passed\n");
    return 0;
}
