#pragma once

#include "storage/RandomAccessReader.h"
#include "voice/experimental/PreparedLoopHandoff.h"
#include <cstring>

namespace gm_synth::app {

// Control/startup only. The owner MUST supply immutable, pinned bytes for the
// lifetime of this reader and every source/parser that uses it. Embedded const
// flash is one such owner; a const pointer into a mutable file buffer is NOT.
// This class neither takes ownership nor claims file/OTA locking or authenticity.
// SHA verifies content against a trusted expected digest; it is not a signature.
class VerifiedReadOnlyBank final : public storage::RandomAccessReader {
public:
    using Digest = std::array<std::uint8_t, 32>;
    using HashFn = bool (*)(const std::uint8_t*, std::size_t, Digest&) noexcept;
    enum class Status { Ok, AlreadyOpen, InvalidInput, InvalidContainer, HashFailed, DigestMismatch };
    VerifiedReadOnlyBank() = default;
    VerifiedReadOnlyBank(const VerifiedReadOnlyBank&) = delete;
    VerifiedReadOnlyBank& operator=(const VerifiedReadOnlyBank&) = delete;

    Status open(const std::uint8_t* immutable_bytes, std::size_t bytes,
                const Digest& expected, std::uint64_t session,
                std::uint32_t bank_generation, HashFn hash,
                std::size_t max_bytes = 64u * 1024u * 1024u) noexcept {
        if (data_) return Status::AlreadyOpen; // no rebinding live parser/source views
        bool nonzero = false;
        for (const auto b : expected) nonzero = nonzero || b != 0;
        if (!immutable_bytes || bytes < 12 || bytes > max_bytes ||
            !session || !bank_generation || !hash || !nonzero) return Status::InvalidInput;
        if (std::memcmp(immutable_bytes, "RIFF", 4) || std::memcmp(immutable_bytes + 8, "sfbk", 4))
            return Status::InvalidContainer;
        std::uint32_t riff_bytes = 0;
        for (unsigned i = 0; i < 4; ++i) riff_bytes |= std::uint32_t{immutable_bytes[4 + i]} << (8 * i);
        if (std::uint64_t{riff_bytes} + 8 != bytes) return Status::InvalidContainer;
        Digest actual{};
        if (!hash(immutable_bytes, bytes, actual)) return Status::HashFailed;
        if (actual != expected) return Status::DigestMismatch;
        asset_ = {actual, session, bank_generation}; // computed digest, not unverified metadata
        bytes_ = bytes;
        data_ = immutable_bytes; // publish locally only after ALL checks
        return Status::Ok;
    }
    bool valid() const noexcept { return data_ != nullptr; }
    voice::experimental::PreparedLoopAsset asset() const noexcept { return asset_; }
    std::uint64_t size() const override { return bytes_; }
    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        if (!data_ || !destination || offset > bytes_ || bytes > bytes_ - offset) return false;
        std::memcpy(destination, data_ + static_cast<std::size_t>(offset), bytes);
        return true;
    }
    // No reset/rebind API: reconstruct the entire stopped owner group for a new
    // asset. Session serial allocation/non-reuse is a loader responsibility.
private:
    const std::uint8_t* data_ = nullptr;
    std::size_t bytes_ = 0;
    voice::experimental::PreparedLoopAsset asset_{};
};
} // namespace gm_synth::app
