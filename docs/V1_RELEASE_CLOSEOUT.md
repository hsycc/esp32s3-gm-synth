# V1 release closeout — M8 frozen, provenance and listening pending

Status: **IN PROGRESS — NOT A RELEASE APPROVAL**.

This checkpoint follows the user's acceptance of the closeout plan after the
three M8 hardware logs. Stop speculative DSP micro-optimization. Do not change
firmware, default gains, event timelines, DMA or polyphony merely to obtain a
better number. Do not merge PR #17/#18 or publish a release while required gates
below remain unresolved.

## Evidence and gates

| Gate | Evidence/status | Required next action |
|---|---|---|
| Automated baseline | `bb31908bf4af5c966e2f17075e54b08d9a3d057c`: previously verified 13/13 CI; documentation-only descendant `0e373edd144ed9a48e76aa33f4290d26bc076999`: independently rechecked 13/13 success | Every new commit needs its own CI result; do not carry an old green status forward |
| M8 physical runtime | Three submitted `bb31908-dirty` logs pass both I2S hard gates, normal 24 / heavy 8 voice envelope, MIDI/CC21 and PCM-checksum checks | Retain evidence; see `benchmarks/M8_PHYSICAL_THREE_RUN_2026-10-24.md` |
| M8 source-to-image provenance | **UNRESOLVED**; only printed ELF hash prefix `8a66d5396` is available | Collect existing local source/build evidence BEFORE rebuilding; review the actual historical inputs |
| M7 provenance | **UNRESOLVED** for the earlier `95aa189-dirty` image | Review separately; a current working tree cannot reconstruct lost historical edits |
| Audible qualification | **NOT RUN / not documented** | Complete `docs/V1_AUDIBLE_ACCEPTANCE.md`; automated pass and PCM hashes are not listening results |
| Output level | Unity master / DAC -12 dB tested for runtime; analog headroom and alternative volumes **NOT QUALIFIED** | Keep baseline; assess the intended release level separately |
| Final integration / release | **BLOCKED** | After provenance, listening and final CI: merge stacked PRs in dependency order, validate the integrated revision, record exact source/config/artifact identity and publish only supported claims |

The 24-voice figure means sample voices, not 24 keys for every layered preset.
The Korvo/ES8311 hardware evidence is mono-downmixed output, not a physical
stereo audition. Host compatibility matrices do not prove arbitrary SF2
compatibility or subjective audio quality.

## A. Collect without modifying the tested checkout

Save `tools/release_evidence.py` outside the original checkout, for example as
`~/Downloads/release_evidence.py`. Do not switch the dirty checkout to obtain it.
Python 3 and Git are sufficient; ESP-IDF need not be activated.

```sh
python3 "$HOME/Downloads/release_evidence.py" \
  --repo "$HOME/work/esp32s3-gm-synth" \
  --expected bb31908bf4af5c966e2f17075e54b08d9a3d057c \
  --output "$HOME/Desktop/m8-source-evidence-$(date +%Y%m%d-%H%M%S).zip"
```

`--log /absolute/path/m8-run1.log` can be repeated for additional logs. Without
it, the tool checks only `reports/m8-physical/m8-run*.log` and
`reports/m7-physical/m7-fix-run*.log` in the specified repository. Missing logs
are recorded; the three already supplied logs remain available separately.

The ZIP contains current HEAD/tree/status, staged and unstaged tracked patches,
checkpoint-to-HEAD changes, existing ELF/application binaries, selected build
metadata and sdkconfig files, SHA-256 hashes, and explicitly selected/found logs.
The raw SF2/FATFS image is **hashed only**, not redistributed. Untracked file
contents, Git remotes/config and environment dumps are not collected. Patches
and binaries can still be sensitive: inspect the ZIP before sharing.

The script does not fetch, checkout, stash, reset, clean, build, flash or upload.
It refuses to overwrite an existing ZIP or write inside the checkout/Git
metadata directories. The expected commit must already exist locally. The
manifest always says `historical_build_provenance=UNRESOLVED` and
`release_approved=false`; successful collection is not a release gate pass.

Do not delete, rebuild or overwrite `build-m7-release` before collection.
A matching truncated hash is a useful cross-check, not proof of complete binary
identity. Today's clean `git status` is not historical build provenance, and a
local ELF hash by itself does not prove what is flashed on the board.

## B. Review and decide the minimum valid follow-up

Review both staged/unstaged changes, expected-to-HEAD changes, generated config,
compiler/build metadata, actual ELF/bin identity and the logs together. Verify
whether historical differences can actually be established, rather than assuming
that the differences observed now are the ones used at build time.

If the historical differences are documented and do not affect compiled sources,
build settings or test assets, record the rationale and preserve the accepted
image evidence. If inputs changed, the local ELF no longer matches the logs, or
historical inputs cannot be reconstructed, establish a clean reproducible
candidate and run the agreed hardware acceptance again. Never remove `-dirty`
from a version string or relax a gate to manufacture equivalence.

Do not demand another three runs *before* this review. Conversely, do not promise
that recollection alone will eliminate the need for a clean-image retest.

## C. Listening and final release

Use the separate listening worksheet. Existing qualification cues and the
GeneralUser loop-seam WAV exporter are available; a full representative-bank
on-device audition is not automatically provided by the benchmark image.
Never describe a host sample WAV as the embedded synth's final DAC output.

Do not enable global crossfade based solely on loop-jump statistics. Samples
604/602/62 each need an explicit audition decision, including pitch/timbre and
loop-period effects. A new runtime fix is a separately tested change, not part
of this evidence-only closeout batch.

The final record must identify the integrated commit, build environment/config,
full artifact hashes, supported bank/profile and limitations, actual listening
observations, and remaining deferred issues. Until then, status remains
**hardware-tested image / conditional source acceptance**, not V1 released.

## Automation coverage

`tests/test_release_evidence.py` is discovered by the existing `host-tests`
Python unittest job. It covers clean/dirty and detached checkouts, staged and
unstaged preservation, identity parsing, missing inputs, safe output locations,
no-clobber behavior and conservative provenance handling. Local execution of
these tests is not an ESP-IDF cross-build or a new physical qualification.
