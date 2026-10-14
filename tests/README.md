# Tests

Planned test layers:

- host/unit tests for MIDI/GM state
- SF2 parser tests
- zone-resolution tests
- sample-lifetime/refcount tests
- event-queue and frame-offset scheduling tests
- deterministic DSP tests where practical
- hardware-only integration tests for I2S, PSRAM and realtime timing

Host tests must not be used as evidence of true ESP32-S3 realtime capacity.