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
        "sf2/Sf2PdtaReader.cpp"
        "sf2/Sf2ResolvedZone.cpp"
        "sf2/Sf2PlaybackMaterializer.cpp"
        "sf2/Sf2PlaybackProgramPreparer.cpp"
        "gm/ProgramChangeMailbox.cpp"
        "gm/PlaybackProgramSnapshot.cpp"
        "gm/ZoneResolver.cpp"
        "voice/PitchMath.cpp"
        "voice/VoiceRenderer.cpp"
        "voice/AudioBlockRenderer.cpp"
        "voice/AdaptivePolyphony.cpp"
        "realtime/RealtimeEventQueue.cpp"
        "realtime/BlockEventScheduler.cpp"
        "realtime/ScheduledAudioBlockRenderer.cpp"
        "realtime/RenderDeadlineMonitor.cpp"
        "realtime/TimedAudioBlockRenderer.cpp"
        "platform/EspPsramArena.cpp"
        "platform/EspStorageBenchmark.cpp"
        "platform/EspRealtimeClock.cpp"
        "platform/EspI2sAudioSink.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_psram esp_timer log esp_driver_i2s
    PRIV_REQUIRES heap
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
