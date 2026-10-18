set(GM_SYNTH_MAIN_SRCS
    "app_main.cpp"
    "ProgramPrepareBenchmark.cpp"
    "RenderTimingBenchmark.cpp"
    "EffectsTimingBenchmark.cpp"
)

set(GM_SYNTH_MAIN_PRIV_REQUIRES fatfs)

if(CONFIG_GM_SYNTH_I2S_QUAL_ENABLE)
    list(APPEND GM_SYNTH_MAIN_SRCS "I2sDmaQualification.cpp")
    list(APPEND GM_SYNTH_MAIN_PRIV_REQUIRES esp_driver_i2s)
else()
    list(APPEND GM_SYNTH_MAIN_SRCS "I2sDmaQualificationDisabled.cpp")
endif()

idf_component_register(
    SRCS ${GM_SYNTH_MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES firmware esp_psram esp_timer
    PRIV_REQUIRES ${GM_SYNTH_MAIN_PRIV_REQUIRES}
)
