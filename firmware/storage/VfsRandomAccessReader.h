#pragma once

#include "RandomAccessReader.h"

#include <cstdio>

namespace gm_synth::storage {

// Random-access reader backed by the C/POSIX file API used by ESP-IDF VFS.
// The reader owns the FILE handle but does not mount or unmount a filesystem.
// Any path already exposed through ESP-IDF VFS (for example FAT/SD or LittleFS)
// can be opened through this class.
class VfsRandomAccessReader final : public RandomAccessReader {
public:
    VfsRandomAccessReader() = default;
    explicit VfsRandomAccessReader(const char* path);
    ~VfsRandomAccessReader() override;

    VfsRandomAccessReader(const VfsRandomAccessReader&) = delete;
    VfsRandomAccessReader& operator=(const VfsRandomAccessReader&) = delete;

    bool open(const char* path);
    void close();

    bool isOpen() const { return file_ != nullptr; }
    std::uint64_t size() const override { return size_; }
    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override;

private:
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
};

}  // namespace gm_synth::storage
