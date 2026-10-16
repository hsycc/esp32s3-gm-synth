idf_component_register(
    SRCS
        "sample/SampleResidencyManager.cpp"
        "sample/SampleResidencySourceLoad.cpp"
        "sample/SampleResidencyView.cpp"
        "sample/PublishedSampleDirectory.cpp"
        "sample/ProgramSnapshot.cpp"
        "sample/ProgramPrepareController.cpp"
        "storage/VfsRandomAccessReader.cpp"
        "storage/StorageReadBenchmark.cpp"
        "sf2/Sf2SampleSource.cpp"
        "gm/ProgramChangeMailbox.cpp"
        "voice/PitchMath.cpp"
        "voice/VoiceRenderer.cpp"
        "voice/AudioBlockRenderer.cpp"
        "voice/AdaptivePolyphony.cpp"
        "platform/EspPsramArena.cpp"
        "platform/EspStorageBenchmark.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_psram esp_timer log
    PRIV_REQUIRES heap
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
