set(GM_SYNTH_MAIN_SRCS
    "app_main.cpp"
    "QualificationIdleMonitor.cpp"
    "ProgramPrepareBenchmark.cpp"
    "RenderTimingBenchmark.cpp"
    "DspStageTimingBenchmark.cpp"
    "EffectsTimingBenchmark.cpp"
)

# ESP-IDF resolves the component dependency graph before sdkconfig-dependent
# source selection is finalized. Keep the Korvo driver components in the
# unconditional dependency graph so feature-on builds receive their public
# include paths reliably. KorvoV4Es8311.h exposes I2C handle types, making
# esp_driver_i2c a public dependency; GPIO and I2S remain implementation-only.
set(GM_SYNTH_MAIN_REQUIRES
    firmware
    esp_psram
    esp_timer
    esp_driver_i2c
)
set(GM_SYNTH_MAIN_PRIV_REQUIRES
    fatfs
    esp_system
    esp_driver_gpio
    esp_driver_i2s
)

if(CONFIG_GM_SYNTH_I2S_QUAL_ENABLE)
    list(APPEND GM_SYNTH_MAIN_SRCS
        "I2sDmaQualification.cpp"
        "I2sWorstBlockProfiler.cpp"
        "KorvoV4Es8311.cpp"
        "PhysicalCleanSf2AudibleGate.cpp"
        "PhysicalMusicStress.cpp"
        "PhysicalHeavyDspStress.cpp"
    )
else()
    list(APPEND GM_SYNTH_MAIN_SRCS "I2sDmaQualificationDisabled.cpp")
endif()

idf_component_register(
    SRCS ${GM_SYNTH_MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${GM_SYNTH_MAIN_REQUIRES}
    PRIV_REQUIRES ${GM_SYNTH_MAIN_PRIV_REQUIRES}
)
