#pragma once

#include "Sf2NoteOnModulator.h"
#include "Sf2PdtaReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

constexpr std::size_t kSf2MaxPreparedNoteOnRoutesPerZone = 8;

struct Sf2PreparedNoteOnRouteBundle {
    std::array<Sf2PreparedNoteOnModulator, kSf2MaxPreparedNoteOnRoutesPerZone> routes{};
    std::uint8_t count = 0;

    bool valid() const {
        if (count > routes.size()) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!routes[index].valid()) {
                return false;
            }
        }
        return true;
    }
};

enum class PreparedNoteOnRouteResolveStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    PresetNotFound,
    ReaderError,
    OutputTooSmall,
};

struct PreparedNoteOnRouteResolveResult {
    PreparedNoteOnRouteResolveStatus status = PreparedNoteOnRouteResolveStatus::InvalidArgument;
    std::size_t zone_count = 0;
    std::size_t required_capacity = 0;
    std::uint16_t preset_index = 0;
    std::uint32_t candidate_records = 0;
    std::uint32_t bundle_overflow_records = 0;
    std::uint16_t max_routes_per_zone = 0;

    bool fullyRepresented() const {
        return status == PreparedNoteOnRouteResolveStatus::Ok &&
            bundle_overflow_records == 0;
    }
};

// Control-domain companion to Sf2ZoneModulatorResolver. It extracts only the
// additional immutable key/velocity routes targeted by the M7 compatibility
// closure, while preserving the exact flattened-zone ordering and SF2
// global/local/preset hierarchy used by the production resolver.
class Sf2PreparedNoteOnRouteResolver {
public:
    PreparedNoteOnRouteResolveStatus resolveProgram(
        Sf2PdtaReader& pdta,
        std::uint16_t bank,
        std::uint16_t program,
        Sf2PreparedNoteOnRouteBundle* output,
        std::size_t output_capacity,
        PreparedNoteOnRouteResolveResult* result);
};

}  // namespace gm_synth::sf2
