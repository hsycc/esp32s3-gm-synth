idf_component_register(
    SRCS
        "sample/SampleResidencyManager.cpp"
        "sample/SampleResidencySourceLoad.cpp"
        "sample/SampleResidencyView.cpp"
        "sample/PublishedSampleDirectory.cpp"
        "sample/ProgramSnapshot.cpp"
        "sample/ProgramPrepareController.cpp"
        "storage/VfsRandomAccessReader.cpp"
        "sf2/Sf2SampleSource.cpp"
        "gm/ProgramChangeMailbox.cpp"
        "platform/EspPsramArena.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_psram
    PRIV_REQUIRES heap
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
