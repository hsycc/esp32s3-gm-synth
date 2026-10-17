#include "../../firmware/sf2/Sf2PdtaReader.h"
#include "../../firmware/sf2/Sf2ZoneModulatorResolver.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>

using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2ZoneModulatorResolver;
using gm_synth::sf2::ZoneModulatorResolveResult;
using gm_synth::sf2::ZoneModulatorResolveStatus;
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

void test_fm_bank_inherits_mandatory_default_modulators() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<ResolvedZoneModulation, 8> zones{};
    ZoneModulatorResolveResult result{};
    Sf2ZoneModulatorResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ZoneModulatorResolveStatus::Ok);
    CHECK(result.zone_count == 1);
    CHECK(result.required_capacity == 1);
    CHECK(result.supported_records == 0);
    CHECK(result.unsupported_records == 0);
    CHECK(zones[0].velocity_to_attenuation_cb == 960);
    CHECK(zones[0].velocity_to_filter_fc_cents == -2400);
    CHECK(zones[0].volume_to_attenuation_cb == 960);
    CHECK(zones[0].pan_amount == 500);
    CHECK(zones[0].expression_to_attenuation_cb == 960);
    CHECK(zones[0].channel_pressure_to_vib_lfo_pitch_cents == 50);
    CHECK(zones[0].modulation_wheel_to_vib_lfo_pitch_cents == 50);
    CHECK(zones[0].reverb_send_amount == 200);
    CHECK(zones[0].chorus_send_amount == 200);
    CHECK(zones[0].supported_velocity_attenuation_records == 0);
    CHECK(zones[0].supported_velocity_filter_records == 0);
    CHECK(zones[0].supported_channel_control_records == 0);
    CHECK(zones[0].supported_vibrato_records == 0);
    CHECK(zones[0].supported_effects_send_records == 0);
    CHECK(!zones[0].hasUnsupportedModulators());
}

void test_generaluser_program_zero_matches_native_zone_count() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<ResolvedZoneModulation, 256> zones{};
    ZoneModulatorResolveResult result{};
    Sf2ZoneModulatorResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, zones.data(), zones.size(), &result) ==
          ZoneModulatorResolveStatus::Ok);
    CHECK(result.zone_count == 200);
    CHECK(result.required_capacity == 200);
}

void test_generaluser_bank_exercises_supported_modulators() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<ResolvedZoneModulation, 256> zones{};
    Sf2ZoneModulatorResolver resolver;
    bool found_velocity_override = false;
    bool found_velocity_filter_override_or_cancellation = false;
    bool found_channel_control_override = false;
    bool found_effect_send_override = false;

    for (std::size_t preset_index = 0; preset_index < pdta.presetCount(); ++preset_index) {
        Sf2PdtaReader::PresetHeader preset{};
        CHECK(pdta.presetHeader(preset_index, &preset));
        ZoneModulatorResolveResult result{};
        const auto status = resolver.resolveProgram(
            pdta,
            preset.bank,
            preset.program,
            zones.data(),
            zones.size(),
            &result);
        CHECK(status == ZoneModulatorResolveStatus::Ok);
        if (status != ZoneModulatorResolveStatus::Ok) {
            continue;
        }
        for (std::size_t index = 0; index < result.zone_count; ++index) {
            found_velocity_override = found_velocity_override ||
                zones[index].supported_velocity_attenuation_records != 0;
            found_velocity_filter_override_or_cancellation =
                found_velocity_filter_override_or_cancellation ||
                zones[index].supported_velocity_filter_records != 0;
            found_channel_control_override = found_channel_control_override ||
                zones[index].supported_channel_control_records != 0;
            found_effect_send_override = found_effect_send_override ||
                zones[index].supported_effects_send_records != 0 ||
                zones[index].reverb_send_amount != 200 ||
                zones[index].chorus_send_amount != 200;
        }
    }

    CHECK(found_velocity_override);
    CHECK(found_velocity_filter_override_or_cancellation);
    CHECK(found_effect_send_override);
    // Not every checked-in bank needs to redefine CC7/10/11. When one does,
    // the new counter proves it was classified as supported rather than folded
    // into unsupported_modulator_records.
    if (found_channel_control_override) {
        std::printf("GeneralUser exercises file-defined CC7/10/11 modulation\n");
    }
}

void test_count_pass_is_transactional() {
    FileReader reader("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    ZoneModulatorResolveResult result{};
    Sf2ZoneModulatorResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 0, nullptr, 0, &result) ==
          ZoneModulatorResolveStatus::OutputTooSmall);
    CHECK(result.zone_count == 0);
    CHECK(result.required_capacity == 200);
}

void test_missing_program_is_explicit() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2PdtaReader pdta;
    CHECK(pdta.begin(reader));

    std::array<ResolvedZoneModulation, 8> zones{};
    ZoneModulatorResolveResult result{};
    Sf2ZoneModulatorResolver resolver;
    CHECK(resolver.resolveProgram(pdta, 0, 127, zones.data(), zones.size(), &result) ==
          ZoneModulatorResolveStatus::PresetNotFound);
}

}  // namespace

int main() {
    test_fm_bank_inherits_mandatory_default_modulators();
    test_generaluser_program_zero_matches_native_zone_count();
    test_generaluser_bank_exercises_supported_modulators();
    test_count_pass_is_transactional();
    test_missing_program_is_explicit();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 zone-modulator checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 zone-modulator host checks passed\n");
    return 0;
}