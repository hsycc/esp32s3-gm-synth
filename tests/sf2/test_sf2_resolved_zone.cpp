#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/sf2/Sf2ResolvedZone.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>

using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::NativeZoneFlagAddressOutsideSampleHeader;
using gm_synth::sf2::NativeZoneFlagInstrumentModulatorsPresent;
using gm_synth::sf2::NativeZoneFlagPresetModulatorsPresent;
using gm_synth::sf2::ResolveProgramResult;
using gm_synth::sf2::ResolveProgramStatus;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2ResolvedZoneResolver;
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

void test_fm_simple_program_matches_host_reference_shape() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> zones{};
    ResolveProgramResult result{};
    Sf2ResolvedZoneResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ResolveProgramStatus::Ok);
    CHECK(result.zone_count == 1);
    CHECK(result.required_capacity == 1);
    CHECK(result.preset_index == 0);
    CHECK(result.invalid_references == 0);
    CHECK(result.unknown_generators == 0);
    CHECK(result.zones_with_modulators == 0);

    const NativeResolvedZone& zone = zones[0];
    CHECK(zone.sample_id == 0);
    CHECK(zone.instrument_id == 0);
    CHECK(zone.bank == 0);
    CHECK(zone.program == 0);
    CHECK(zone.sample_rate == 44100);
    CHECK(zone.sample_root_key == 60);
    CHECK(zone.sample_pitch_correction_cents == 0);
    CHECK(zone.rootKey() == 60);
    CHECK(zone.sample_start == static_cast<std::int64_t>(zone.shdr_start));
    CHECK(zone.sample_end == static_cast<std::int64_t>(zone.shdr_end));
    CHECK(zone.sample_end - zone.sample_start == 63945);
    CHECK(zone.key_low <= zone.key_high);
    CHECK(zone.velocity_low <= zone.velocity_high);
    CHECK(zone.preset_modulator_records == 0);
    CHECK(zone.instrument_modulator_records == 0);
}

void test_generaluser_zone_counts_match_analyzer_manifest() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    struct Case {
        std::uint16_t program;
        std::size_t expected_zones;
        std::uint16_t expected_preset;
    };
    const std::array<Case, 6> cases{{
        {0, 200, 0},
        {8, 5, 2},
        {40, 145, 10},
        {63, 144, 15},
        {80, 128, 18},
        {124, 3, 34},
    }};

    std::array<NativeResolvedZone, 256> zones{};
    Sf2ResolvedZoneResolver resolver;
    for (const Case& test : cases) {
        ResolveProgramResult result{};
        CHECK(resolver.resolveProgram(
                  pdta,
                  0,
                  test.program,
                  zones.data(),
                  zones.size(),
                  &result) == ResolveProgramStatus::Ok);
        CHECK(result.zone_count == test.expected_zones);
        CHECK(result.required_capacity == test.expected_zones);
        CHECK(result.preset_index == test.expected_preset);
        CHECK(result.invalid_references == 0);
        for (std::size_t index = 0; index < result.zone_count; ++index) {
            CHECK(zones[index].sample_id < pdta.sampleCount());
            CHECK(zones[index].key_low <= zones[index].key_high);
            CHECK(zones[index].velocity_low <= zones[index].velocity_high);
            CHECK(zones[index].sample_start < zones[index].sample_end);
        }
    }
}

void test_generaluser_reports_modulators_instead_of_dropping_them() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<NativeResolvedZone, 256> zones{};
    ResolveProgramResult result{};
    Sf2ResolvedZoneResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ResolveProgramStatus::Ok);
    CHECK(result.zone_count == 200);
    CHECK(result.zones_with_modulators > 0);

    bool found_modulator_flag = false;
    for (std::size_t index = 0; index < result.zone_count; ++index) {
        const std::uint32_t flags = zones[index].flags;
        if ((flags & (NativeZoneFlagPresetModulatorsPresent |
                      NativeZoneFlagInstrumentModulatorsPresent)) != 0) {
            found_modulator_flag = true;
            CHECK(zones[index].preset_modulator_records != 0 ||
                  zones[index].instrument_modulator_records != 0);
        }
    }
    CHECK(found_modulator_flag);
}

void test_output_capacity_is_reported_transactionally() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<NativeResolvedZone, 64> zones{};
    ResolveProgramResult result{};
    Sf2ResolvedZoneResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ResolveProgramStatus::OutputTooSmall);
    CHECK(result.zone_count == 0);
    CHECK(result.required_capacity == 200);

    ResolveProgramResult count_only{};
    CHECK(resolver.resolveProgram(pdta, 0, 0, nullptr, 0, &count_only) ==
          ResolveProgramStatus::OutputTooSmall);
    CHECK(count_only.zone_count == 0);
    CHECK(count_only.required_capacity == 200);
}

void test_missing_program_is_explicit() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<NativeResolvedZone, 8> zones{};
    ResolveProgramResult result{};
    Sf2ResolvedZoneResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 127, zones.data(), zones.size(), &result) ==
          ResolveProgramStatus::PresetNotFound);
    CHECK(result.zone_count == 0);
    CHECK(result.required_capacity == 0);
}

void test_real_bank_address_offsets_remain_a_native_qualification_signal() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<NativeResolvedZone, 256> zones{};
    ResolveProgramResult result{};
    Sf2ResolvedZoneResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ResolveProgramStatus::Ok);

    // The resolver must preserve the exact resolved absolute addresses. Whether
    // a zone can be represented by the current resident sample is deliberately
    // a later materialization decision rather than silently clamping here.
    for (std::size_t index = 0; index < result.zone_count; ++index) {
        const auto& zone = zones[index];
        if ((zone.flags & NativeZoneFlagAddressOutsideSampleHeader) == 0) {
            CHECK(zone.sample_start >= static_cast<std::int64_t>(zone.shdr_start));
            CHECK(zone.sample_end <= static_cast<std::int64_t>(zone.shdr_end));
        }
    }
}

}  // namespace

int main() {
    test_fm_simple_program_matches_host_reference_shape();
    test_generaluser_zone_counts_match_analyzer_manifest();
    test_generaluser_reports_modulators_instead_of_dropping_them();
    test_output_capacity_is_reported_transactionally();
    test_missing_program_is_explicit();
    test_real_bank_address_offsets_remain_a_native_qualification_signal();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 resolved-zone checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 resolved-zone host checks passed\n");
    return 0;
}
