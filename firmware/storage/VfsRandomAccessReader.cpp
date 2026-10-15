#include "VfsRandomAccessReader.h"

#include <climits>

namespace gm_synth::storage {

VfsRandomAccessReader::VfsRandomAccessReader(const char* path) {
    open(path);
}

VfsRandomAccessReader::~VfsRandomAccessReader() {
    close();
}

bool VfsRandomAccessReader::open(const char* path) {
    close();

    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    file_ = std::fopen(path, "rb");
    if (file_ == nullptr) {
        return false;
    }

    if (std::fseek(file_, 0, SEEK_END) != 0) {
        close();
        return false;
    }

    const long end = std::ftell(file_);
    if (end < 0) {
        close();
        return false;
    }

    size_ = static_cast<std::uint64_t>(end);
    std::clearerr(file_);
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        close();
        return false;
    }

    return true;
}

void VfsRandomAccessReader::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    size_ = 0;
}

bool VfsRandomAccessReader::readAt(
    std::uint64_t offset,
    void* destination,
    std::size_t bytes) {
    if (file_ == nullptr || offset > size_ || bytes > size_ - offset) {
        return false;
    }
    if (bytes == 0) {
        return true;
    }
    if (destination == nullptr || offset > static_cast<std::uint64_t>(LONG_MAX)) {
        return false;
    }

    std::clearerr(file_);
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }

    return std::fread(destination, 1, bytes, file_) == bytes;
}

}  // namespace gm_synth::storage
