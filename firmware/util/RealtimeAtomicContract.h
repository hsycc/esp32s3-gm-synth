#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_memory_utils.h"
#if defined(CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND) && CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND
#include "esp_idf_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !CONFIG_IDF_TARGET_ESP32S3 || !defined(__XTENSA__) || defined(__clang__)
#error "Address-qualified atomic contract is audited only for ESP32-S3 GCC"
#endif
static_assert(ESP_IDF_VERSION_MAJOR == 6 && ESP_IDF_VERSION_MINOR == 1,
              "Re-audit the IDF atomic backend before changing SDK series");
#endif
#endif

namespace gm_synth::util {

static_assert(sizeof(std::atomic<std::uint32_t>) == 4,
              "State words must use the four-byte atomic backend");
static_assert(alignof(std::atomic<std::uint32_t>) >= 4,
              "State words must be naturally aligned");
#if !defined(ESP_PLATFORM) || !defined(CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND) || !CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Unsupported target: lock-free 32-bit state words are required");
#endif

// No dereference or atomic operation: also usable before placement construction.
// Restrict ESP state to ordinary DRAM, not RTC/IRAM/reclaimed DROM aliases. The
// IDF 6.1 workaround tests the VIRTUAL external-RAM range, not heap capability.
// Therefore esp_ptr_internal() alone is insufficient for every cache layout.
inline bool realtimeAtomic32StorageAllowed(const void* storage) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(storage);
    if (!begin || begin % alignof(std::atomic<std::uint32_t>) != 0 ||
        begin > std::numeric_limits<std::uintptr_t>::max() - 3u) return false;
#if defined(ESP_PLATFORM)
    const auto end = begin + 3u;
    if (!esp_ptr_in_dram(storage) ||
        !esp_ptr_in_dram(reinterpret_cast<const void*>(end))) return false;
    // Match esp_libc's routing test, including aliases labelled internal by the
    // heap. Never let these two handoff words take its external spinlock path.
    if (begin < SOC_EXTRAM_DATA_HIGH && end >= SOC_EXTRAM_DATA_LOW) return false;
#endif
    return true;
}

// Construction only, before publication. These owners are nonmovable. Keep the
// std::atomic storage AND every existing load/store memory order unchanged.
// Abort is deliberate and remains active under NDEBUG: silently using a lock
// for an incorrectly placed realtime object is not a supported fallback.
inline void requireRealtimeAtomic32(const std::atomic<std::uint32_t>& word) noexcept {
    if (!realtimeAtomic32StorageAllowed(&word)) std::abort();
}

} // namespace gm_synth::util
