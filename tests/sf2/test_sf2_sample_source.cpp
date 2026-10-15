#include "../../firmware/sample/ProgramPrepareController.h"
#include "../../firmware/sf2/Sf2SampleSource.h"
#include "../../firmware/storage/RandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using gm_synth::sample::ProgramPrepareController;
using gm_synth::sample::ProgramSnapshotExchange;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sample::SampleId;
using gm_synth::sample::SampleResidencyManager;
using gm_synth::sf2::Sf2SampleSource;
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

void test_real_bank_sample_counts() {
    struct Case {
        const char* path;
        std::size_t expected_samples;
    };

    const std::array<Case, 3> cases{{
        {"assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2", 67},
        {"assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2", 186},
        {"assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2", 102},
    }};

    for (const Case& test : cases) {
        FileReader reader(test.path);
        CHECK(reader.valid());
        Sf2SampleSource source;
        CHECK(source.begin(reader));
        CHECK(source.sampleCount() == test.expected_samples);
        CHECK(source.smplPayloadOffset() != 0);

        SampleDescriptor first{};
        CHECK(source.descriptorFor(0, &first));
        CHECK(first.id == 0);
        CHECK(first.pcm_bytes != 0);
        CHECK((first.pcm_bytes & 1u) == 0u);
        CHECK(first.sample_rate != 0);
        CHECK(source.sourceByteOffset(0) >= source.smplPayloadOffset());
    }
}

void test_fm_gm_descriptor_matches_analyzer_baseline() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2SampleSource source;
    CHECK(source.begin(reader));

    SampleDescriptor sample0{};
    CHECK(source.descriptorFor(0, &sample0));
    CHECK(sample0.pcm_bytes == 127890);
    CHECK(sample0.frames == 63945);
    CHECK(sample0.sample_rate == 44100);
    CHECK(sample0.root_key == 60);
    CHECK(sample0.pitch_correction == 0);
}

void test_program_prepare_reads_real_pcm_directly_into_arena() {
    FileReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.valid());
    Sf2SampleSource source;
    CHECK(source.begin(reader));

    SampleDescriptor descriptor{};
    CHECK(source.descriptorFor(0, &descriptor));
    const std::size_t aligned_bytes = SampleResidencyManager::alignedBytes(descriptor.pcm_bytes);
    std::vector<std::uint32_t> arena_words((aligned_bytes + 1024 + 3) / 4, 0);

    SampleResidencyManager residency;
    PublishedSampleDirectory directory;
    ProgramSnapshotExchange exchange;
    ProgramPrepareController controller(residency, directory, exchange, &source);

    CHECK(residency.begin(arena_words.data(), arena_words.size() * sizeof(std::uint32_t)));
    const std::array<SampleId, 1> dependencies{{0}};
    CHECK(controller.activateInitialProgram(
        0, 0, 0, 1000, dependencies.data(), dependencies.size()));

    const auto view = residency.residentView(0);
    CHECK(view.valid());
    CHECK(view.descriptor == descriptor);
    CHECK(view.pin_count == 1);
    CHECK(directory.published(0) != nullptr);
    CHECK(residency.stats().used_bytes == aligned_bytes);
    CHECK(controller.stats().samples_loaded == 1);
    CHECK(controller.stats().sample_bytes_loaded == descriptor.pcm_bytes);

    constexpr std::size_t kProbeBytes = 64;
    std::array<std::uint8_t, kProbeBytes> expected_begin{};
    std::array<std::uint8_t, kProbeBytes> expected_end{};
    CHECK(reader.readAt(source.sourceByteOffset(0), expected_begin.data(), expected_begin.size()));
    CHECK(reader.readAt(
        source.sourceByteOffset(0) + descriptor.pcm_bytes - expected_end.size(),
        expected_end.data(),
        expected_end.size()));

    const auto* resident_bytes = reinterpret_cast<const std::uint8_t*>(view.pcm);
    CHECK(std::memcmp(resident_bytes, expected_begin.data(), expected_begin.size()) == 0);
    CHECK(std::memcmp(
        resident_bytes + descriptor.pcm_bytes - expected_end.size(),
        expected_end.data(),
        expected_end.size()) == 0);
}

void test_invalid_riff_is_rejected() {
    class MemoryReader final : public RandomAccessReader {
    public:
        std::uint64_t size() const override { return bytes.size(); }
        bool readAt(std::uint64_t offset, void* destination, std::size_t count) override {
            if (offset > bytes.size() || count > bytes.size() - offset) {
                return false;
            }
            std::memcpy(destination, bytes.data() + offset, count);
            return true;
        }
        std::array<std::uint8_t, 12> bytes{{'N','O','P','E',4,0,0,0,'s','f','b','k'}};
    } reader;

    Sf2SampleSource source;
    CHECK(!source.begin(reader));
    CHECK(source.lastError() == Sf2SampleSource::Error::InvalidRiff);
}

}  // namespace

int main() {
    test_real_bank_sample_counts();
    test_fm_gm_descriptor_matches_analyzer_baseline();
    test_program_prepare_reads_real_pcm_directly_into_arena();
    test_invalid_riff_is_rejected();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 sample-source checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 sample-source host checks passed\n");
    return 0;
}
