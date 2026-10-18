set(GM_SYNTH_MAIN_SRCS
    "app_main.cpp"
    "ProgramPrepareBenchmark.cpp"
    "RenderTimingBenchmark.cpp"
    "EffectsTimingBenchmark.cpp"
)

set(GM_SYNTH_MAIN_REQUIRES
    firmware
    esp_psram
    esp_timer
)
set(GM_SYNTH_MAIN_PRIV_REQUIRES fatfs)

if(CONFIG_GM_SYNTH_I2S_QUAL_ENABLE)
    list(APPEND GM_SYNTH_MAIN_SRCS
        "I2sDmaQualification.cpp"
        "KorvoV4Es8311.cpp"
    )

    # KorvoV4Es8311.h exposes ESP-IDF I2C handle types, so esp_driver_i2c is a
    # public component dependency. GPIO and I2S are implementation-only.
    list(APPEND GM_SYNTH_MAIN_REQUIRES esp_driver_i2c)
    list(APPEND GM_SYNTH_MAIN_PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
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
