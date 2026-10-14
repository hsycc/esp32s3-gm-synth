# SF2 Analyzer

Host-side tool planned for M1.

The analyzer exists to answer concrete compatibility and memory questions about the fixed GM SoundFont before firmware architecture depends on assumptions.

Expected outputs:
- RIFF/SF2 chunk structure
- `smpl` payload size
- presets / instruments / samples
- generator usage
- modulator usage
- linked/stereo sample usage
- per-program sample dependencies
- per-program PCM working-set size
- representative zone/layer overlap
- drum requirements
- JSON report for tests/CI

The tool should run identically in local development and Codex Cloud where possible.

Do not commit proprietary or redistributability-unclear `.sf2` banks to this repository.