# Development Workflow

## One repository, two execution environments

The project should remain usable from both:
- local development machines connected to physical ESP32-S3 hardware
- Codex Cloud for code changes, host tooling, builds, tests and review

The repository is the source of truth. Environment-specific knowledge should be minimized and documented.

## Codex Cloud responsibilities

Good Cloud tasks:
- refactoring
- host-side SF2 analysis
- unit tests
- static analysis
- build-system work
- documentation
- review/diff analysis
- deterministic fixture generation

Cloud results must not be treated as proof of physical ESP32-S3 realtime performance.

## Local responsibilities

Local/hardware tasks:
- firmware flashing
- physical I2S/DAC verification
- PSRAM behavior measurements
- block timing on ESP32-S3
- underrun/overrun validation
- audible glitch/voice-steal evaluation
- long-running board stress tests

## Shared command surface

Once M0 selects the toolchain baseline, implement stable scripts under `scripts/` so both local and Cloud tasks use the same entry points:

```text
./scripts/setup.sh
./scripts/build.sh
./scripts/test.sh
./scripts/analyze_sf2.sh <path-to-bank.sf2>
```

Avoid instructions that only work through a particular IDE button sequence.

## Hardware feedback loop

Recommended cycle:

```text
Codex/local code change
        ↓
host build + tests
        ↓
commit / PR
        ↓
local flash to ESP32-S3
        ↓
hardware benchmark/log capture
        ↓
commit benchmark artifact or summarized report
        ↓
next engineering iteration
```

## Codex task guidance

For substantial work, point Codex to:
- root `AGENTS.md`
- `.agent/PLANS.md`
- the relevant `tasks/M*.md`
- architecture documents under `docs/`

Do not ask Codex to optimize realtime code without a before/after measurement plan.
