idf_component_register(
    SRCS
        "app_main.cpp"
        "ProgramPrepareBenchmark.cpp"
        "RenderTimingBenchmark.cpp"
        "EffectsTimingBenchmark.cpp"
    INCLUDE_DIRS "."
    REQUIRES firmware esp_psram esp_timer
    PRIV_REQUIRES fatfs
)
