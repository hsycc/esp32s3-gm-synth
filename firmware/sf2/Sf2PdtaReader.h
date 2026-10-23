#pragma once

#include "../storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

// Random-access view over the SoundFont pdta tables. The reader keeps only
// chunk offsets/counts in RAM; individual records are decoded on demand in the
// control domain. This avoids making SF2 metadata size a fixed SRAM/PSRAM cost.
class Sf2PdtaReader {
public:
    static constexpr std::size_t kNameBytes = 20;

    struct PresetHeader {
        std::array<char, kNameBytes + 1u> name{};
        std::uint16_t program = 0;
        std::uint16_t bank = 0;
        std::uint16_t bag_index = 0;
        std::uint32_t library = 0;
        std::uint32_t genre = 0;
        std::uint32_t morphology = 0;
    };

    struct Bag {
        std::uint16_t generator_index = 0;
        std::uint16_t modulator_index = 0;
    };

    struct Generator {
        std::uint16_t oper = 0;
        std::uint16_t amount = 0;

        std::int16_t signedAmount() const {
            return static_cast<std::int16_t>(amount);
        }
        std::uint8_t rangeLow() const {
            return static_cast<std::uint8_t>(amount & 0xffu);
        }
        std::uint8_t rangeHigh() const {
            return static_cast<std::uint8_t>((amount >> 8u) & 0xffu);
        }
    };

    struct Modulator {
        std::uint16_t source_oper = 0;
        std::uint16_t destination_oper = 0;
        std::int16_t amount = 0;
        std::uint16_t amount_source_oper = 0;
        std::uint16_t transform_oper = 0;
    };

    struct InstrumentHeader {
        std::array<char, kNameBytes + 1u> name{};
        std::uint16_t bag_index = 0;
    };

    struct SampleHeader {
        std::array<char, kNameBytes + 1u> name{};
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::uint32_t start_loop = 0;
        std::uint32_t end_loop = 0;
        std::uint32_t sample_rate = 0;
        std::uint8_t original_pitch = 60;
        std::int8_t pitch_correction = 0;
        std::uint16_t sample_link = 0;
        std::uint16_t sample_type = 0;

        bool isRom() const { return (sample_type & 0x8000u) != 0u; }
    };

    enum class Error : std::uint8_t {
        None = 0,
        ReaderTooSmall,
        InvalidRiff,
        MissingSmpl,
        MissingPdtaChunk,
        InvalidTableSize,
        MissingTerminalRecord,
        InvalidIndexRange,
        InvalidSampleRange,
        ReadFailed,
    };

    bool begin(storage::RandomAccessReader& reader);

    // Counts exclude terminal phdr/inst/shdr records where indicated.
    std::size_t presetCount() const { return phdr_.record_count == 0 ? 0 : phdr_.record_count - 1u; }
    std::size_t instrumentCount() const { return inst_.record_count == 0 ? 0 : inst_.record_count - 1u; }
    std::size_t sampleCount() const { return shdr_.record_count == 0 ? 0 : shdr_.record_count - 1u; }

    std::size_t presetHeaderRecordCount() const { return phdr_.record_count; }
    std::size_t presetBagRecordCount() const { return pbag_.record_count; }
    std::size_t presetModulatorRecordCount() const { return pmod_.record_count; }
    std::size_t presetGeneratorRecordCount() const { return pgen_.record_count; }
    std::size_t instrumentHeaderRecordCount() const { return inst_.record_count; }
    std::size_t instrumentBagRecordCount() const { return ibag_.record_count; }
    std::size_t instrumentModulatorRecordCount() const { return imod_.record_count; }
    std::size_t instrumentGeneratorRecordCount() const { return igen_.record_count; }
    std::size_t sampleHeaderRecordCount() const { return shdr_.record_count; }

    std::uint64_t smplPayloadOffset() const { return smpl_payload_offset_; }
    std::uint32_t smplPayloadBytes() const { return smpl_payload_bytes_; }
    std::uint64_t smplPointCount() const { return smpl_payload_bytes_ / 2u; }

    bool presetHeader(std::size_t index, PresetHeader* out);
    bool presetBag(std::size_t index, Bag* out);
    bool presetModulator(std::size_t index, Modulator* out);
    bool presetGenerator(std::size_t index, Generator* out);
    bool instrumentHeader(std::size_t index, InstrumentHeader* out);
    bool instrumentBag(std::size_t index, Bag* out);
    bool instrumentModulator(std::size_t index, Modulator* out);
    bool instrumentGenerator(std::size_t index, Generator* out);
    bool sampleHeader(std::size_t index, SampleHeader* out);

    Error lastError() const { return last_error_; }

private:
    struct Table {
        std::uint64_t offset = 0;
        std::uint32_t bytes = 0;
        std::size_t record_count = 0;
    };

    bool locateChunks();
    bool scanList(
        std::uint64_t list_payload_offset,
        std::uint32_t list_payload_size,
        const char expected_type[4]);
    bool assignTable(Table* table, std::uint64_t offset, std::uint32_t bytes);
    bool validateTables();
    bool validateStructure();
    bool validatePresetStructure();
    bool validateInstrumentStructure();
    bool validateSampleHeaders();

    bool readExact(std::uint64_t offset, void* destination, std::size_t bytes);
    bool readRecord(
        const Table& table,
        std::size_t index,
        std::size_t record_bytes,
        void* destination);
    void setError(Error error) { last_error_ = error; }

    storage::RandomAccessReader* reader_ = nullptr;
    std::uint64_t riff_end_ = 0;
    std::uint64_t smpl_payload_offset_ = 0;
    std::uint32_t smpl_payload_bytes_ = 0;

    Table phdr_{};
    Table pbag_{};
    Table pmod_{};
    Table pgen_{};
    Table inst_{};
    Table ibag_{};
    Table imod_{};
    Table igen_{};
    Table shdr_{};

    Error last_error_ = Error::None;
};

}  // namespace gm_synth::sf2
