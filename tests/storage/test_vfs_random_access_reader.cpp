#include "../../firmware/sf2/Sf2SampleSource.h"
#include "../../firmware/storage/VfsRandomAccessReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>

using gm_synth::sf2::Sf2SampleSource;
using gm_synth::storage::VfsRandomAccessReader;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

void write_fixture(const char* path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    for (std::uint32_t i = 0; i < 4096; ++i) {
        const std::uint8_t value = static_cast<std::uint8_t>((i * 37u + 11u) & 0xffu);
        out.write(reinterpret_cast<const char*>(&value), 1);
    }
    CHECK(out.good());
}

void test_basic_random_access_and_bounds() {
    constexpr const char* kPath = "build/host-tests/vfs_reader_fixture.bin";
    write_fixture(kPath);

    VfsRandomAccessReader reader;
    CHECK(!reader.isOpen());
    CHECK(reader.size() == 0);
    CHECK(!reader.open(nullptr));
    CHECK(!reader.open(""));
    CHECK(reader.open(kPath));
    CHECK(reader.isOpen());
    CHECK(reader.size() == 4096);

    std::array<std::uint8_t, 32> bytes{};
    CHECK(reader.readAt(100, bytes.data(), bytes.size()));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto expected = static_cast<std::uint8_t>(((100u + i) * 37u + 11u) & 0xffu);
        CHECK(bytes[i] == expected);
    }

    CHECK(reader.readAt(reader.size(), nullptr, 0));
    CHECK(!reader.readAt(reader.size(), bytes.data(), 1));
    CHECK(!reader.readAt(reader.size() + 1, bytes.data(), 0));
    CHECK(!reader.readAt(0, nullptr, 1));

    reader.close();
    CHECK(!reader.isOpen());
    CHECK(reader.size() == 0);
    CHECK(!reader.readAt(0, bytes.data(), 1));
}

void test_real_sf2_vfs_chain() {
    VfsRandomAccessReader reader("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2");
    CHECK(reader.isOpen());
    CHECK(reader.size() > 12);

    Sf2SampleSource source;
    CHECK(source.begin(reader));
    CHECK(source.sampleCount() == 67);
    CHECK(source.smplPayloadOffset() != 0);

    gm_synth::sample::SampleDescriptor descriptor{};
    CHECK(source.descriptorFor(0, &descriptor));
    CHECK(descriptor.pcm_bytes == 127890);

    std::array<std::uint8_t, 64> probe{};
    CHECK(reader.readAt(source.sourceByteOffset(0), probe.data(), probe.size()));
}

}  // namespace

int main() {
    test_basic_random_access_and_bounds();
    test_real_sf2_vfs_chain();

    if (failures != 0) {
        std::fprintf(stderr, "%d VFS reader checks failed\n", failures);
        return 1;
    }

    std::printf("VFS RandomAccessReader host checks passed\n");
    return 0;
}
