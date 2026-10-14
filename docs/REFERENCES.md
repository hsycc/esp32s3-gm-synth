# Reference Projects

These repositories are engineering references. The target architecture intentionally combines selected ideas rather than adopting any one project wholesale.

## 1. copych/ESP32_SF2_Sampler_Synthesizer

Repository:
- https://github.com/copych/ESP32_SF2_Sampler_Synthesizer

Primary value:
- closest match to the actual GM/SF2 goal
- SF2 RIFF/PDTA/SDTA parsing
- preset/instrument/zone resolution
- GM/MIDI channel semantics
- PSRAM sample-pool approach
- sample voice playback, interpolation, envelopes, filters
- global FX send/mix architecture
- dual-core realtime/control split

Important limitations/risks to verify rather than inherit blindly:
- parser is not a full SF2 specification implementation
- current implementation does not parse full preset/instrument modulator tables (`pmod`/`imod`)
- some modulation behavior is simplified/approximated for MCU cost
- Program Change can require a safe audio pause while sample residency changes
- some module responsibilities are concentrated in a large Synth object

Use as the primary behavioral/algorithmic prototype for the first executable baseline.

## 2. copych/ESP32_SD_Sampler

Repository:
- https://github.com/copych/ESP32_SD_Sampler

Primary value:
- direct SD/MMC sample streaming architecture
- large sample-set scalability
- per-voice streaming/refill thinking
- adaptive polyphony based on observed pressure
- fast-release / click-reduced voice stealing
- underrun and runtime diagnostics
- realistic ESP32-S3 sample-playback performance experience

For this project, streaming is not a V1 requirement for a ~14 MB GM bank. The preferred reuse is the overload-control and graceful-degradation strategy.

Potential future use:
- very large piano/orchestral expansion banks
- optional StreamSource implementation behind the same sample interface used by resident PCM

## 3. RealTimeAudioLab/RTAL-EAI-004-Phoenix-Sound-Sampler

Repository:
- https://github.com/RealTimeAudioLab/RTAL-EAI-004-Phoenix-Sound-Sampler

Primary value:
- complete standalone ESP32-S3 instrument architecture
- explicit realtime task separation
- timestamped MIDI/sequence scheduling
- intra-block event placement
- measured audio deadline budget
- persistent diagnostics and stress validation
- SRAM/PSRAM placement based on actual measurements
- robust state/session persistence patterns
- larger product-level decomposition than the copych examples

Phoenix is not an SF2/GM engine, so do not copy its sample model as the GM domain model. Reuse its realtime-system and validation methodology.

## Combined design intent

The target system should approximately inherit:

| Concern | Primary reference |
|---|---|
| SF2 parsing | ESP32_SF2_Sampler_Synthesizer |
| GM 16-channel semantics | ESP32_SF2_Sampler_Synthesizer |
| Zone resolution | ESP32_SF2_Sampler_Synthesizer |
| PSRAM sample pool | ESP32_SF2_Sampler_Synthesizer |
| Voice DSP | ESP32_SF2_Sampler_Synthesizer |
| Adaptive polyphony | ESP32_SD_Sampler |
| Smooth overload/stealing behavior | ESP32_SD_Sampler |
| Future SD streaming | ESP32_SD_Sampler |
| Timestamped event queue | Phoenix |
| Intra-block scheduling | Phoenix |
| Realtime task model | Phoenix |
| Deadline monitoring | Phoenix |
| Memory placement methodology | Phoenix |
| Stress/release validation | Phoenix |

## Principle

Reference implementations are evidence and starting points, not specifications. Any important performance or compatibility claim must be re-measured on this project's selected hardware and validated SoundFont.
