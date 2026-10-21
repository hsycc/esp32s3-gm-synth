#pragma once

#include "Sf2FilterMath.h"
#include "Sf2ModulationMath.h"
#include "Sf2NoteOnModulatorSet.h"
#include "Sf2NoteOnModulatorTargets.h"
#include "Sf2PdtaReader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sf2 {

struct ResolvedZoneModulationStorage {
    std::int32_t velocity_to_attenuation_cb = kSf2DefaultVelocityAttenuationCb;
    std::int32_t velocity_to_filter_fc_cents = kSf2DefaultVelocityFilterCutoffCents;
    // Additional NoteOn-velocity routes exercised by real GM SoundFonts.
    std::int32_t velocity_positive_to_mod_env_filter_cents = 0;
    std::int32_t velocity_negative_to_mod_env_filter_cents = 0;
    Sf2FilterQNoteOnModulation velocity_to_filter_q_cb{};
    std::int32_t velocity_to_mod_env_attack_timecents = 0;
    std::int32_t velocity_to_vol_env_attack_timecents = 0;

    // Generic immutable NoteOn key/velocity routes to initialFilterFc. The
    // default negative-linear velocity route remains in its dedicated field so
    // existing replacement semantics stay bit-for-bit compatible; other
    // direction/polarity/curve combinations are represented here instead of
    // adding a field for every source-oper identity.
    std::array<Sf2NoteOnSourceRoute, kSf2MaxNoteOnSourceRoutesPerDestination>
        initial_filter_fc_note_on_routes{};
    std::uint8_t initial_filter_fc_note_on_route_count = 0;

    // M7 compatibility closure telemetry for prepared dual-source FilterQ
    // routes. The actual route payload is carried by velocity_to_filter_q_cb.
    std::uint16_t supported_prepared_note_on_records = 0;

    std::int32_t volume_to_attenuation_cb = kSf2DefaultControllerAttenuationCb;
    std::int32_t pan_amount = kSf2DefaultPanAmount;
    std::int32_t expression_to_attenuation_cb = kSf2DefaultControllerAttenuationCb;
    std::int32_t channel_pressure_to_vib_lfo_pitch_cents =
        kSf2DefaultVibratoControllerAmountCents;
    std::int32_t modulation_wheel_to_vib_lfo_pitch_cents =
        kSf2DefaultVibratoControllerAmountCents;
    std::int32_t reverb_send_amount = kSf2DefaultEffectsSendAmount;
    std::int32_t chorus_send_amount = kSf2DefaultEffectsSendAmount;
    std::int32_t pitch_wheel_to_pitch_cents = 12700;

    std::uint16_t supported_velocity_attenuation_records = 0;
    std::uint16_t supported_velocity_filter_records = 0;
    std::uint16_t supported_velocity_dsp_records = 0;
    std::uint16_t supported_note_on_source_records = 0;
    std::uint16_t supported_channel_control_records = 0;
    std::uint16_t supported_vibrato_records = 0;
    std::uint16_t supported_effects_send_records = 0;
    std::uint16_t unsupported_modulator_records = 0;


};

struct ResolvedZoneModulation : ResolvedZoneModulationStorage {
    // Alias the generic prepared-route name used by the production resolver to
    // the payload carried through the existing FilterQ field. This keeps the
    // current resolver implementation unchanged while Materializer copies the
    // exact same payload into PlaybackZone.
    Sf2PreparedNoteOnModulatorSet& prepared_note_on_routes;

    ResolvedZoneModulation()
        : prepared_note_on_routes(velocity_to_filter_q_cb.additional_routes) {}

    ResolvedZoneModulation(const ResolvedZoneModulation& other)
        : ResolvedZoneModulationStorage(other),
          prepared_note_on_routes(velocity_to_filter_q_cb.additional_routes) {}

    ResolvedZoneModulation& operator=(const ResolvedZoneModulation& other) {
        if (this != &other) {
            ResolvedZoneModulationStorage::operator=(other);
        }
        return *this;
    }

    bool hasUnsupportedModulators() const {
        return unsupported_modulator_records != 0 ||
            !velocity_to_filter_q_cb.fullyRepresentedFilterQOnly();
    }

    bool fullySupported() const { return !hasUnsupportedModulators(); }
};

enum class ZoneModulatorResolveStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    PresetNotFound,
    ReaderError,
    OutputTooSmall,
};

struct ZoneModulatorResolveResult {
    ZoneModulatorResolveStatus status = ZoneModulatorResolveStatus::InvalidArgument;
    std::size_t zone_count = 0;
    std::size_t required_capacity = 0;
    std::uint16_t preset_index = 0;
    std::uint32_t supported_records = 0;
    std::uint32_t unsupported_records = 0;

    bool fullySupported() const {
        return status == ZoneModulatorResolveStatus::Ok && unsupported_records == 0;
    }
};

class Sf2ZoneModulatorResolver {
public:
    ZoneModulatorResolveStatus resolveProgram(
        Sf2PdtaReader& pdta,
        std::uint16_t bank,
        std::uint16_t program,
        ResolvedZoneModulation* output,
        std::size_t output_capacity,
        ZoneModulatorResolveResult* result);
};

}  // namespace gm_synth::sf2
