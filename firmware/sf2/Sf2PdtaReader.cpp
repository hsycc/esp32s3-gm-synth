#include "Sf2PdtaReader.h"

#include <cstring>

namespace gm_synth::sf2 {
namespace {

std::uint16_t readLe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8u);
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

std::int16_t signedLe16(const std::uint8_t* bytes) {
    const std::uint16_t raw = readLe16(bytes);
    if ((raw & 0x8000u) == 0u) {
        return static_cast<std::int16_t>(raw);
    }
    return static_cast<std::int16_t>(static_cast<std::int32_t>(raw) - 0x10000);
}

std::int8_t signedByte(std::uint8_t raw) {
    if ((raw & 0x80u) == 0u) {
        return static_cast<std::int8_t>(raw);
    }
    return static_cast<std::int8_t>(static_cast<std::int16_t>(raw) - 0x100);
}

bool fourccEquals(const std::uint8_t* bytes, const char text[4]) {
    return std::memcmp(bytes, text, 4) == 0;
}

bool textFourccEquals(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

void copyName(
    std::array<char, Sf2PdtaReader::kNameBytes + 1u>* destination,
    const std::uint8_t* source) {
    destination->fill('\0');
    std::memcpy(destination->data(), source, Sf2PdtaReader::kNameBytes);
}

}  // namespace

bool Sf2PdtaReader::readExact(
    std::uint64_t offset,
    void* destination,
    std::size_t bytes) {
    if (reader_ == nullptr || destination == nullptr ||
        offset > reader_->size() || bytes > reader_->size() - offset) {
        return false;
    }
    return reader_->readAt(offset, destination, bytes);
}

bool Sf2PdtaReader::readRecord(
    const Table& table,
    std::size_t index,
    std::size_t record_bytes,
    void* destination) {
    if (destination == nullptr || index >= table.record_count || record_bytes == 0) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    const std::uint64_t relative = static_cast<std::uint64_t>(index) * record_bytes;
    if (relative > table.bytes || record_bytes > table.bytes - relative) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    if (!readExact(table.offset + relative, destination, record_bytes)) {
        setError(Error::ReadFailed);
        return false;
    }
    return true;
}

bool Sf2PdtaReader::begin(storage::RandomAccessReader& reader) {
    reader_ = &reader;
    riff_end_ = 0;
    smpl_payload_offset_ = 0;
    smpl_payload_bytes_ = 0;
    phdr_ = Table{};
    pbag_ = Table{};
    pmod_ = Table{};
    pgen_ = Table{};
    inst_ = Table{};
    ibag_ = Table{};
    imod_ = Table{};
    igen_ = Table{};
    shdr_ = Table{};
    setError(Error::None);

    if (reader.size() < 12) {
        setError(Error::ReaderTooSmall);
        return false;
    }

    std::uint8_t header[12]{};
    if (!readExact(0, header, sizeof(header))) {
        setError(Error::ReadFailed);
        return false;
    }
    if (!fourccEquals(header, "RIFF") || !fourccEquals(header + 8, "sfbk")) {
        setError(Error::InvalidRiff);
        return false;
    }

    const std::uint64_t riff_size = readLe32(header + 4);
    if (riff_size < 4 || riff_size > reader.size() - 8u) {
        setError(Error::InvalidRiff);
        return false;
    }
    riff_end_ = 8u + riff_size;

    if (!locateChunks() || !validateTables() || !validateStructure()) {
        return false;
    }

    setError(Error::None);
    return true;
}

bool Sf2PdtaReader::locateChunks() {
    std::uint64_t cursor = 12;
    while (cursor + 8u <= riff_end_) {
        std::uint8_t chunk_header[8]{};
        if (!readExact(cursor, chunk_header, sizeof(chunk_header))) {
            setError(Error::ReadFailed);
            return false;
        }

        const std::uint32_t chunk_size = readLe32(chunk_header + 4);
        const std::uint64_t payload = cursor + 8u;
        if (payload > riff_end_ || chunk_size > riff_end_ - payload) {
            setError(Error::InvalidRiff);
            return false;
        }
        const std::uint64_t payload_end = payload + chunk_size;

        if (fourccEquals(chunk_header, "LIST")) {
            if (chunk_size < 4u) {
                setError(Error::InvalidRiff);
                return false;
            }
            std::uint8_t list_type[4]{};
            if (!readExact(payload, list_type, sizeof(list_type))) {
                setError(Error::ReadFailed);
                return false;
            }
            if (fourccEquals(list_type, "sdta")) {
                if (!scanList(payload, chunk_size, "sdta")) {
                    return false;
                }
            } else if (fourccEquals(list_type, "pdta")) {
                if (!scanList(payload, chunk_size, "pdta")) {
                    return false;
                }
            }
        }

        cursor = payload_end + (chunk_size & 1u);
    }
    return true;
}

bool Sf2PdtaReader::assignTable(
    Table* table,
    std::uint64_t offset,
    std::uint32_t bytes) {
    if (table == nullptr) {
        setError(Error::InvalidRiff);
        return false;
    }
    if (table->offset != 0 || table->bytes != 0) {
        setError(Error::InvalidRiff);
        return false;
    }
    table->offset = offset;
    table->bytes = bytes;
    return true;
}

bool Sf2PdtaReader::scanList(
    std::uint64_t list_payload_offset,
    std::uint32_t list_payload_size,
    const char expected_type[4]) {
    if (list_payload_size < 4u) {
        setError(Error::InvalidRiff);
        return false;
    }

    std::uint8_t list_type[4]{};
    if (!readExact(list_payload_offset, list_type, sizeof(list_type))) {
        setError(Error::ReadFailed);
        return false;
    }
    if (!fourccEquals(list_type, expected_type)) {
        setError(Error::InvalidRiff);
        return false;
    }

    std::uint64_t cursor = list_payload_offset + 4u;
    const std::uint64_t list_end = list_payload_offset + list_payload_size;
    while (cursor + 8u <= list_end) {
        std::uint8_t child_header[8]{};
        if (!readExact(cursor, child_header, sizeof(child_header))) {
            setError(Error::ReadFailed);
            return false;
        }
        const std::uint32_t child_size = readLe32(child_header + 4);
        const std::uint64_t payload = cursor + 8u;
        if (payload > list_end || child_size > list_end - payload) {
            setError(Error::InvalidRiff);
            return false;
        }
        const std::uint64_t payload_end = payload + child_size;

        if (textFourccEquals(expected_type, "sdta")) {
            if (fourccEquals(child_header, "smpl")) {
                if (smpl_payload_offset_ != 0 || smpl_payload_bytes_ != 0) {
                    setError(Error::InvalidRiff);
                    return false;
                }
                smpl_payload_offset_ = payload;
                smpl_payload_bytes_ = child_size;
            }
        } else if (textFourccEquals(expected_type, "pdta")) {
            Table* table = nullptr;
            if (fourccEquals(child_header, "phdr")) {
                table = &phdr_;
            } else if (fourccEquals(child_header, "pbag")) {
                table = &pbag_;
            } else if (fourccEquals(child_header, "pmod")) {
                table = &pmod_;
            } else if (fourccEquals(child_header, "pgen")) {
                table = &pgen_;
            } else if (fourccEquals(child_header, "inst")) {
                table = &inst_;
            } else if (fourccEquals(child_header, "ibag")) {
                table = &ibag_;
            } else if (fourccEquals(child_header, "imod")) {
                table = &imod_;
            } else if (fourccEquals(child_header, "igen")) {
                table = &igen_;
            } else if (fourccEquals(child_header, "shdr")) {
                table = &shdr_;
            }
            if (table != nullptr && !assignTable(table, payload, child_size)) {
                return false;
            }
        }

        cursor = payload_end + (child_size & 1u);
    }
    return true;
}

bool Sf2PdtaReader::validateTables() {
    if (smpl_payload_offset_ == 0 || smpl_payload_bytes_ == 0) {
        setError(Error::MissingSmpl);
        return false;
    }
    if ((smpl_payload_bytes_ & 1u) != 0u) {
        setError(Error::InvalidTableSize);
        return false;
    }

    struct Requirement {
        Table* table;
        std::size_t record_bytes;
    };
    const Requirement requirements[] = {
        {&phdr_, 38u},
        {&pbag_, 4u},
        {&pmod_, 10u},
        {&pgen_, 4u},
        {&inst_, 22u},
        {&ibag_, 4u},
        {&imod_, 10u},
        {&igen_, 4u},
        {&shdr_, 46u},
    };

    for (const Requirement& requirement : requirements) {
        if (requirement.table->offset == 0 || requirement.table->bytes == 0) {
            setError(Error::MissingPdtaChunk);
            return false;
        }
        if ((requirement.table->bytes % requirement.record_bytes) != 0u) {
            setError(Error::InvalidTableSize);
            return false;
        }
        requirement.table->record_count = requirement.table->bytes / requirement.record_bytes;
        if (requirement.table->record_count == 0) {
            setError(Error::MissingTerminalRecord);
            return false;
        }
    }
    return true;
}

bool Sf2PdtaReader::validateStructure() {
    if (!validatePresetStructure() || !validateInstrumentStructure() ||
        !validateSampleHeaders()) {
        return false;
    }
    return true;
}

bool Sf2PdtaReader::validatePresetStructure() {
    PresetHeader previous_header{};
    bool have_previous_header = false;
    for (std::size_t index = 0; index < phdr_.record_count; ++index) {
        PresetHeader header{};
        if (!presetHeader(index, &header)) {
            return false;
        }
        if (header.bag_index >= pbag_.record_count) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        if (have_previous_header && previous_header.bag_index > header.bag_index) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        previous_header = header;
        have_previous_header = true;
    }

    Bag previous_bag{};
    bool have_previous_bag = false;
    for (std::size_t index = 0; index < pbag_.record_count; ++index) {
        Bag bag{};
        if (!presetBag(index, &bag)) {
            return false;
        }
        if (bag.generator_index > pgen_.record_count ||
            bag.modulator_index > pmod_.record_count) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        if (have_previous_bag &&
            (previous_bag.generator_index > bag.generator_index ||
             previous_bag.modulator_index > bag.modulator_index)) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        previous_bag = bag;
        have_previous_bag = true;
    }
    return true;
}

bool Sf2PdtaReader::validateInstrumentStructure() {
    InstrumentHeader previous_header{};
    bool have_previous_header = false;
    for (std::size_t index = 0; index < inst_.record_count; ++index) {
        InstrumentHeader header{};
        if (!instrumentHeader(index, &header)) {
            return false;
        }
        if (header.bag_index >= ibag_.record_count) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        if (have_previous_header && previous_header.bag_index > header.bag_index) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        previous_header = header;
        have_previous_header = true;
    }

    Bag previous_bag{};
    bool have_previous_bag = false;
    for (std::size_t index = 0; index < ibag_.record_count; ++index) {
        Bag bag{};
        if (!instrumentBag(index, &bag)) {
            return false;
        }
        if (bag.generator_index > igen_.record_count ||
            bag.modulator_index > imod_.record_count) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        if (have_previous_bag &&
            (previous_bag.generator_index > bag.generator_index ||
             previous_bag.modulator_index > bag.modulator_index)) {
            setError(Error::InvalidIndexRange);
            return false;
        }
        previous_bag = bag;
        have_previous_bag = true;
    }
    return true;
}

bool Sf2PdtaReader::validateSampleHeaders() {
    if (shdr_.record_count == 0) {
        setError(Error::MissingTerminalRecord);
        return false;
    }

    const std::uint64_t smpl_points = smplPointCount();
    for (std::size_t index = 0; index + 1u < shdr_.record_count; ++index) {
        SampleHeader sample{};
        if (!sampleHeader(index, &sample)) {
            return false;
        }
        if (sample.start >= sample.end || sample.end > smpl_points || sample.sample_rate == 0) {
            setError(Error::InvalidSampleRange);
            return false;
        }
    }
    return true;
}

bool Sf2PdtaReader::presetHeader(std::size_t index, PresetHeader* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[38]{};
    if (!readRecord(phdr_, index, sizeof(raw), raw)) {
        return false;
    }
    PresetHeader decoded{};
    copyName(&decoded.name, raw);
    decoded.program = readLe16(raw + 20);
    decoded.bank = readLe16(raw + 22);
    decoded.bag_index = readLe16(raw + 24);
    decoded.library = readLe32(raw + 26);
    decoded.genre = readLe32(raw + 30);
    decoded.morphology = readLe32(raw + 34);
    *out = decoded;
    return true;
}

bool Sf2PdtaReader::presetBag(std::size_t index, Bag* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[4]{};
    if (!readRecord(pbag_, index, sizeof(raw), raw)) {
        return false;
    }
    out->generator_index = readLe16(raw);
    out->modulator_index = readLe16(raw + 2);
    return true;
}

bool Sf2PdtaReader::presetModulator(std::size_t index, Modulator* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[10]{};
    if (!readRecord(pmod_, index, sizeof(raw), raw)) {
        return false;
    }
    out->source_oper = readLe16(raw);
    out->destination_oper = readLe16(raw + 2);
    out->amount = signedLe16(raw + 4);
    out->amount_source_oper = readLe16(raw + 6);
    out->transform_oper = readLe16(raw + 8);
    return true;
}

bool Sf2PdtaReader::presetGenerator(std::size_t index, Generator* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[4]{};
    if (!readRecord(pgen_, index, sizeof(raw), raw)) {
        return false;
    }
    out->oper = readLe16(raw);
    out->amount = readLe16(raw + 2);
    return true;
}

bool Sf2PdtaReader::instrumentHeader(std::size_t index, InstrumentHeader* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[22]{};
    if (!readRecord(inst_, index, sizeof(raw), raw)) {
        return false;
    }
    InstrumentHeader decoded{};
    copyName(&decoded.name, raw);
    decoded.bag_index = readLe16(raw + 20);
    *out = decoded;
    return true;
}

bool Sf2PdtaReader::instrumentBag(std::size_t index, Bag* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[4]{};
    if (!readRecord(ibag_, index, sizeof(raw), raw)) {
        return false;
    }
    out->generator_index = readLe16(raw);
    out->modulator_index = readLe16(raw + 2);
    return true;
}

bool Sf2PdtaReader::instrumentModulator(std::size_t index, Modulator* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[10]{};
    if (!readRecord(imod_, index, sizeof(raw), raw)) {
        return false;
    }
    out->source_oper = readLe16(raw);
    out->destination_oper = readLe16(raw + 2);
    out->amount = signedLe16(raw + 4);
    out->amount_source_oper = readLe16(raw + 6);
    out->transform_oper = readLe16(raw + 8);
    return true;
}

bool Sf2PdtaReader::instrumentGenerator(std::size_t index, Generator* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[4]{};
    if (!readRecord(igen_, index, sizeof(raw), raw)) {
        return false;
    }
    out->oper = readLe16(raw);
    out->amount = readLe16(raw + 2);
    return true;
}

bool Sf2PdtaReader::sampleHeader(std::size_t index, SampleHeader* out) {
    if (out == nullptr) {
        setError(Error::InvalidIndexRange);
        return false;
    }
    std::uint8_t raw[46]{};
    if (!readRecord(shdr_, index, sizeof(raw), raw)) {
        return false;
    }
    SampleHeader decoded{};
    copyName(&decoded.name, raw);
    decoded.start = readLe32(raw + 20);
    decoded.end = readLe32(raw + 24);
    decoded.start_loop = readLe32(raw + 28);
    decoded.end_loop = readLe32(raw + 32);
    decoded.sample_rate = readLe32(raw + 36);
    decoded.original_pitch = raw[40];
    decoded.pitch_correction = signedByte(raw[41]);
    decoded.sample_link = readLe16(raw + 42);
    decoded.sample_type = readLe16(raw + 44);
    *out = decoded;
    return true;
}

}  // namespace gm_synth::sf2
