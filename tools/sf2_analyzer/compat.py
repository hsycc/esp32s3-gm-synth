from __future__ import annotations

from typing import Optional


# FluidSynth and legacy EMU-compatible engines scale SoundFont
# initialAttenuation generator values at load time. This is deliberately kept
# out of ResolvedZone.generators_native so host analysis still exposes the
# literal SoundFont hierarchy result.
EMU_ATTENUATION_FACTOR = 0.4
GEN_INITIAL_ATTENUATION = 48

# Compatibility profiles are explicit so a future strict-SF2 mode can coexist
# with the real-world bank behavior expected by GeneralUser/FluidR3-class
# SoundFonts.
STRICT_SF2 = "strict_sf2"
REALWORLD_SF2 = "realworld_sf2"
DEFAULT_RUNTIME_COMPATIBILITY = REALWORLD_SF2


def runtime_generator_base(
    operator: int,
    native_value: Optional[float],
    *,
    profile: str = DEFAULT_RUNTIME_COMPATIBILITY,
) -> Optional[float]:
    """Convert a resolved native generator into the runtime base value.

    `native_value` is the SoundFont generator-hierarchy result before dynamic
    modulators. The real-world profile mirrors the long-standing EMU/FluidSynth
    attenuation compatibility behavior while leaving all other generators in
    native SF2 units.
    """
    if native_value is None:
        return None
    value = float(native_value)
    if profile == STRICT_SF2:
        return value
    if profile != REALWORLD_SF2:
        raise ValueError(f"unknown SF2 compatibility profile {profile!r}")
    if operator == GEN_INITIAL_ATTENUATION:
        return value * EMU_ATTENUATION_FACTOR
    return value
