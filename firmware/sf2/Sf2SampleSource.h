#pragma once

#include "../sample/SampleSource.h"
#include "../storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

class Sf2SampleSource final : public sample::SampleSource {
public:
    static constexpr std::size_t kMaxSamples = sample::SampleResidencyManager::kMaxResidentSamples;
    static constexpr std::size_t kReadChunkBytes = 32u * 1024u;

    enum class Error : std::uint8_t {
        None = 0,
        ReaderTooSmall,
        InvalidRiff,
        MissingSmpl,
        MissingShdr,
        InvalidShdr,
        TooManySamples,
        InvalidSampleRange,
        RomSampleUnsupported,
        ReadFailed,
    };

    bool begin(storage::RandomAccessReader& reader);

    bool descriptorFor(sample::SampleId sample_id, sample::SampleDescriptor* descriptor) override;
    bool fillSample(
        sample::SampleId sample_id,
        std::uint8_t* destination,
        std::size_t pcm_bytes) override;

    std::size_t sampleCount() const { return sample_count_; }
    std::uint64_t smplPayloadOffset() const { return smpl_payload_offset_; }
    std::uint64_t sourceByteOffset(sample::SampleId sample_id) const;
    Error lastError() const { return last_error_; }

private:
    struct SampleRecord {
        bool valid = false;
        bool rom = false;
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::uint32_t start_loop = 0;
        std::uint32_t end_loop = 0;
        std::uint32_t sample_rate = 0;
        std::uint8_t original_pitch = 60;
        std::int8_t pitch_correction = 0;
        std::uint16_t sample_link = 0;
        std::uint16_t sample_type = 0;
    };

    bool locateChunks();
    bool scanList(
        std::uint64_t list_payload_offset,
        std::uint32_t list_payload_size,
        const char expected_type[4]);
    bool parseSampleHeaders();
    bool readExact(std::uint64_t offset, void* destination, std::size_t bytes);
    void setError(Error error) { last_error_ = error; }

    storage::RandomAccessReader* reader_ = nullptr;
    std::uint64_t riff_end_ = 0;
    std::uint64_t smpl_payload_offset_ = 0;
    std::uint32_t smpl_payload_bytes_ = 0;
    std::uint64_t shdr_payload_offset_ = 0;
    std::uint32_t shdr_payload_bytes_ = 0;
    std::array<SampleRecord, kMaxSamples> samples_{};
    std::size_t sample_count_ = 0;
    Error last_error_ = Error::None;
};

}  // namespace gm_synth::sf2
