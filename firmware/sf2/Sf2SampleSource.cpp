#include "Sf2SampleSource.h"

#include <algorithm>
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

bool fourccEquals(const std::uint8_t* bytes, const char text[4]) {
    return std::memcmp(bytes, text, 4) == 0;
}

bool textFourccEquals(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

}  // namespace

bool Sf2SampleSource::readExact(
    std::uint64_t offset,
    void* destination,
    std::size_t bytes) {
    if (reader_ == nullptr || destination == nullptr ||
        offset > reader_->size() || bytes > reader_->size() - offset) {
        return false;
    }
    return reader_->readAt(offset, destination, bytes);
}

bool Sf2SampleSource::begin(storage::RandomAccessReader& reader) {
    reader_ = &reader;
    riff_end_ = 0;
    smpl_payload_offset_ = 0;
    smpl_payload_bytes_ = 0;
    shdr_payload_offset_ = 0;
    shdr_payload_bytes_ = 0;
    sample_count_ = 0;
    samples_.fill(SampleRecord{});
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
    if (riff_size < 4 || riff_size > reader.size() - 8) {
        setError(Error::InvalidRiff);
        return false;
    }
    riff_end_ = 8 + riff_size;

    if (!locateChunks()) {
        return false;
    }
    if (smpl_payload_offset_ == 0 || smpl_payload_bytes_ == 0) {
        setError(Error::MissingSmpl);
        return false;
    }
    if (shdr_payload_offset_ == 0 || shdr_payload_bytes_ == 0) {
        setError(Error::MissingShdr);
        return false;
    }
    if (!parseSampleHeaders()) {
        return false;
    }

    setError(Error::None);
    return true;
}

bool Sf2SampleSource::locateChunks() {
    std::uint64_t cursor = 12;
    while (cursor + 8 <= riff_end_) {
        std::uint8_t chunk_header[8]{};
        if (!readExact(cursor, chunk_header, sizeof(chunk_header))) {
            setError(Error::ReadFailed);
            return false;
        }

        const std::uint32_t chunk_size = readLe32(chunk_header + 4);
        const std::uint64_t payload = cursor + 8;
        const std::uint64_t payload_end = payload + chunk_size;
        if (payload_end > riff_end_) {
            setError(Error::InvalidRiff);
            return false;
        }

        if (fourccEquals(chunk_header, "LIST") && chunk_size >= 4) {
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

bool Sf2SampleSource::scanList(
    std::uint64_t list_payload_offset,
    std::uint32_t list_payload_size,
    const char expected_type[4]) {
    if (list_payload_size < 4) {
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

    std::uint64_t cursor = list_payload_offset + 4;
    const std::uint64_t list_end = list_payload_offset + list_payload_size;
    while (cursor + 8 <= list_end) {
        std::uint8_t child_header[8]{};
        if (!readExact(cursor, child_header, sizeof(child_header))) {
            setError(Error::ReadFailed);
            return false;
        }
        const std::uint32_t child_size = readLe32(child_header + 4);
        const std::uint64_t payload = cursor + 8;
        const std::uint64_t payload_end = payload + child_size;
        if (payload_end > list_end) {
            setError(Error::InvalidRiff);
            return false;
        }

        if (textFourccEquals(expected_type, "sdta") && fourccEquals(child_header, "smpl")) {
            smpl_payload_offset_ = payload;
            smpl_payload_bytes_ = child_size;
        } else if (
            textFourccEquals(expected_type, "pdta") && fourccEquals(child_header, "shdr")) {
            shdr_payload_offset_ = payload;
            shdr_payload_bytes_ = child_size;
        }

        cursor = payload_end + (child_size & 1u);
    }
    return true;
}

bool Sf2SampleSource::parseSampleHeaders() {
    constexpr std::size_t kShdrBytes = 46;
    if (shdr_payload_bytes_ < kShdrBytes || (shdr_payload_bytes_ % kShdrBytes) != 0) {
        setError(Error::InvalidShdr);
        return false;
    }

    const std::size_t record_count = shdr_payload_bytes_ / kShdrBytes;
    if (record_count == 0) {
        setError(Error::InvalidShdr);
        return false;
    }
    const std::size_t playable_count = record_count - 1;  // terminal EOS record
    if (playable_count > samples_.size()) {
        setError(Error::TooManySamples);
        return false;
    }

    const std::uint64_t smpl_points = smpl_payload_bytes_ / 2u;
    for (std::size_t index = 0; index < playable_count; ++index) {
        std::uint8_t raw[kShdrBytes]{};
        if (!readExact(shdr_payload_offset_ + index * kShdrBytes, raw, sizeof(raw))) {
            setError(Error::ReadFailed);
            return false;
        }

        SampleRecord record{};
        record.start = readLe32(raw + 20);
        record.end = readLe32(raw + 24);
        record.start_loop = readLe32(raw + 28);
        record.end_loop = readLe32(raw + 32);
        record.sample_rate = readLe32(raw + 36);
        record.original_pitch = raw[40];
        record.pitch_correction = static_cast<std::int8_t>(raw[41]);
        record.sample_link = readLe16(raw + 42);
        record.sample_type = readLe16(raw + 44);
        record.rom = (record.sample_type & 0x8000u) != 0u;

        if (record.end <= record.start || record.end > smpl_points || record.sample_rate == 0) {
            setError(Error::InvalidSampleRange);
            return false;
        }

        record.valid = true;
        samples_[index] = record;
    }

    sample_count_ = playable_count;
    return true;
}

bool Sf2SampleSource::descriptorFor(
    sample::SampleId sample_id,
    sample::SampleDescriptor* descriptor) {
    if (descriptor == nullptr || static_cast<std::size_t>(sample_id) >= sample_count_) {
        return false;
    }

    const SampleRecord& record = samples_[sample_id];
    if (!record.valid) {
        setError(Error::InvalidSampleRange);
        return false;
    }
    if (record.rom) {
        setError(Error::RomSampleUnsupported);
        return false;
    }

    sample::SampleDescriptor out{};
    out.id = sample_id;
    out.frames = record.end - record.start;
    out.pcm_bytes = out.frames * 2u;
    out.sample_rate = record.sample_rate;
    out.root_key = record.original_pitch;
    out.pitch_correction = record.pitch_correction;
    out.flags = record.sample_type;

    if (record.start_loop >= record.start && record.end_loop <= record.end &&
        record.start_loop < record.end_loop) {
        out.loop_start = record.start_loop - record.start;
        out.loop_end = record.end_loop - record.start;
    }

    *descriptor = out;
    setError(Error::None);
    return true;
}

std::uint64_t Sf2SampleSource::sourceByteOffset(sample::SampleId sample_id) const {
    if (static_cast<std::size_t>(sample_id) >= sample_count_ || !samples_[sample_id].valid) {
        return 0;
    }
    return smpl_payload_offset_ + static_cast<std::uint64_t>(samples_[sample_id].start) * 2u;
}

bool Sf2SampleSource::fillSample(
    sample::SampleId sample_id,
    std::uint8_t* destination,
    std::size_t pcm_bytes) {
    if (destination == nullptr || reader_ == nullptr) {
        return false;
    }

    sample::SampleDescriptor descriptor{};
    if (!descriptorFor(sample_id, &descriptor) || descriptor.pcm_bytes != pcm_bytes) {
        return false;
    }

    const std::uint64_t source_offset = sourceByteOffset(sample_id);
    std::size_t copied = 0;
    while (copied < pcm_bytes) {
        const std::size_t chunk = std::min(kReadChunkBytes, pcm_bytes - copied);
        if (!readExact(source_offset + copied, destination + copied, chunk)) {
            setError(Error::ReadFailed);
            return false;
        }
        copied += chunk;
    }

    setError(Error::None);
    return true;
}

}  // namespace gm_synth::sf2
