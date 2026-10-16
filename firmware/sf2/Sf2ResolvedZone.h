#pragma once

#include "Sf2PdtaReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

constexpr std::size_t kSf2GeneratorCount = 61;

// Generator state after SoundFont instrument replacement + preset offset
// semantics have been applied, still in native SF2 units. value_mask keeps the
// distinction between a numeric zero and an optional generator that remains
// unspecified (notably keynum, velocity and overridingRootKey).
struct NativeGeneratorState {
    std::array<std::int32_t, kSf2GeneratorCount> values{};
    std::uint64_t value_mask = 0;

    bool has(std::uint16_t oper) const {
        return oper < kSf2GeneratorCount &&
               (value_mask & (std::uint64_t{1} << oper)) != 0;
    }

    std::int32_t value(std::uint16_t oper, std::int32_t fallback = 0) const {
        return has(oper) ? values[oper] : fallback;
    }

    void set(std::uint16_t oper, std::int32_t value) {
        if (oper >= kSf2GeneratorCount) {
            return;
        }
        values[oper] = value;
        value_mask |= std::uint64_t{1} << oper;
    }
};

enum NativeZoneFlag : std::uint32_t {
    NativeZoneFlagNone = 0,
    // A preset contained a generator which SF2 forbids at preset level. It is
    // intentionally ignored for numeric merging, matching the host reference.
    NativeZoneFlagIgnoredInvalidPresetGenerator = 1u << 0,
    // A generator operator above endOper (60) was encountered.
    NativeZoneFlagUnknownGenerator = 1u << 1,
    // Resolved absolute sample start/end does not fit the sdta/smpl payload.
    NativeZoneFlagSampleAddressOutOfSmpl = 1u << 2,
    // A looping sample mode has invalid resolved loop boundaries.
    NativeZoneFlagInvalidLoopRange = 1u << 3,
    // Resolved address generators expand outside this shdr sample's original
    // start/end. The current resident-sample representation cannot materialize
    // such a zone without widening residency.
    NativeZoneFlagAddressOutsideSampleHeader = 1u << 4,
    NativeZoneFlagRootKeyOutOfRange = 1u << 5,
    NativeZoneFlagRomSample = 1u << 6,
    NativeZoneFlagStereoOrLinkedSample = 1u << 7,
    // Presence is preserved because even zero-amount modulators may suppress
    // or replace default SoundFont modulators. They must not be silently lost.
    NativeZoneFlagPresetModulatorsPresent = 1u << 8,
    NativeZoneFlagInstrumentModulatorsPresent = 1u << 9,
};

struct NativeResolvedZone {
    std::uint16_t preset_index = 0;
    std::uint16_t bank = 0;
    std::uint16_t program = 0;
    std::uint16_t instrument_id = 0;
    std::uint16_t sample_id = 0;

    std::uint8_t key_low = 0;
    std::uint8_t key_high = 127;
    std::uint8_t velocity_low = 0;
    std::uint8_t velocity_high = 127;

    // Absolute indices into sdta/smpl, matching tools/sf2_analyzer/resolved_zone.py.
    std::int64_t sample_start = 0;
    std::int64_t sample_end = 0;
    std::int64_t loop_start = 0;
    std::int64_t loop_end = 0;

    // Original shdr metadata is retained so a later runtime materializer can
    // determine exactly which semantics fit the resident SampleDescriptor.
    std::uint32_t shdr_start = 0;
    std::uint32_t shdr_end = 0;
    std::uint32_t shdr_loop_start = 0;
    std::uint32_t shdr_loop_end = 0;
    std::uint32_t sample_rate = 0;
    std::uint8_t sample_root_key = 60;
    std::int8_t sample_pitch_correction_cents = 0;
    std::uint16_t sample_link = 0;
    std::uint16_t sample_type = 0;

    NativeGeneratorState generators{};
    std::uint64_t ignored_preset_generator_mask = 0;
    std::uint16_t preset_modulator_records = 0;
    std::uint16_t instrument_modulator_records = 0;
    std::uint32_t flags = NativeZoneFlagNone;

    std::int32_t rootKey() const {
        return generators.has(58) ? generators.value(58) : sample_root_key;
    }
    std::int32_t coarseTuneSemitones() const { return generators.value(51); }
    std::int32_t fineTuneCents() const { return generators.value(52); }
    std::int32_t scaleTuningCentsPerKey() const { return generators.value(56, 100); }
    std::int32_t initialAttenuationCb() const { return generators.value(48); }
    std::int32_t panSf2Units() const { return generators.value(17); }
    std::int32_t initialFilterFcCents() const { return generators.value(8, 13500); }
    std::int32_t initialFilterQCb() const { return generators.value(9); }
    std::int32_t sampleModes() const { return generators.value(54); }
    std::int32_t exclusiveClass() const { return generators.value(57); }
};

enum class ResolveProgramStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    PresetNotFound,
    ReaderError,
    OutputTooSmall,
};

struct ResolveProgramResult {
    ResolveProgramStatus status = ResolveProgramStatus::InvalidArgument;
    std::size_t zone_count = 0;
    std::size_t required_capacity = 0;
    std::uint16_t preset_index = 0;
    std::uint32_t invalid_references = 0;
    std::uint32_t unknown_generators = 0;
    std::uint32_t zones_with_modulators = 0;
};

// Flattens preset -> instrument -> sample zones using the same generator
// hierarchy as the Python host reference. This is a control-domain operation;
// it performs bounded random reads from the SF2 source and never belongs in the
// audio callback.
class Sf2ResolvedZoneResolver {
public:
    ResolveProgramStatus resolveProgram(
        Sf2PdtaReader& pdta,
        std::uint16_t bank,
        std::uint16_t program,
        NativeResolvedZone* output,
        std::size_t output_capacity,
        ResolveProgramResult* result);
};

}  // namespace gm_synth::sf2
