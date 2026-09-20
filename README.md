# Athena Consciousness R26.1

A source overlay for [stock Athena](https://github.com/igorbarshteyn/athena).
Install this repository **after the base Athena installation is complete**.
Keep the two repositories in separate, non-nested directories. This repository
contains the R26.1 runtime sources, the installation entry point, and the
consciousness changelog. It does not contain models or a replacement memory store.

R26.1 provides the native consciousness controller, private reflection and dreams,
metacognitive reporting, sustained cognitive projects, source-aware memory and
camera recall, recoverable consolidation, and coordinated speech delivery/prosody.
The runtime and bundled vision sources are the R26.1 release sources; packaging
does not alter their behavior.

## Repository contents

| Path | Purpose |
| --- | --- |
| `README.md` | Installation, operation, verification and rollback instructions. |
| `install-overlay.sh` | Entry point; delegates to the installer in `patches/`. |
| `changes-consciousness.md` | Preserved consciousness development changelog. |
| `patches/talk-llama/` | Brain, consciousness, memory, emotion calibration and complete nested `mtmd/` vision sources. |
| `patches/whisper-common/` | Matching shared audio capture implementation and header. |
| `patches/orpheus/` | Native speech engine, prosody and speech protocol headers. |
| `patches/speak-daemon.sh` | Matching speech wrapper. |
| `patches/launch-athena-397b.sh` | Optional supervised launcher; review its model, audio and GPU settings. |
| `patches/athena-vision-check.sh` | Camera/model diagnostic helper. |
| `patches/install-consciousness.sh` | Installer implementation; also accepts the documented options directly. |

Historical reports, test workspaces, transcripts, sample memory, release archives
and development tools are not part of this overlay repository. The changelog is
a historical record; use this README for installation.

[Athena R26.1 AI Bill of Materials](AIBOM.md)

## Prerequisites

Use the existing Linux/Bash Athena installation and its working compiler, CMake,
SDL2/libcurl development packages, CUDA toolkit, ONNX Runtime, models and GPU
drivers. The script also uses the usual GNU file utilities, Git, `flock` and
`timeout`; the optional launcher requires `setsid`. Full installs also use `curl`
and `sha256sum` to fetch and verify the missing vision projector (about 922 MB).
It installs no packages and requests no `sudo` access. Other models must already
be installed by the base installer or provided separately.

The tested base is [Athena commit
`e747c0362c4e1c3b95756a57154d09b8df7811ce`](https://github.com/igorbarshteyn/athena/commit/e747c0362c4e1c3b95756a57154d09b8df7811ce).
Its `install.sh` pins Whisper to
`afa2ea544fb4b0448916b4a31ecd33c8685bd482`. The overlay checks both the pin in
the stock installer and the installed Whisper checkout before copying anything.
It never fetches, resets or checks out a dependency. If the check fails, preserve
local edits and resolve the dependency mismatch first. `--allow-drift` is an
explicit, unverified compatibility experiment.

Stop Athena normally and let shutdown finish. Keep a separate backup of your
personal memory before the first R26.1 session. The installer backs up source and
build files; it does not back up or change personal memory.

## Install onto the existing base

Extract this tree or clone your overlay repository beside the stock checkout.
Place the extracted directory's contents at the root of the separate GitHub
repository; do not copy the repository wholesale into stock Athena.
For example, from the overlay directory, `../athena` can be the stock root:

```bash
cd /absolute/path/to/athena-consciousness-r26.1-overlay

# Preview the complete installation without changing Athena.
./install-overlay.sh --athena ../athena --dry-run

# Install the missing vision projector and sources; rebuild all three applications.
./install-overlay.sh --athena ../athena --jobs 4

# Independently check the installed files and applications.
./install-overlay.sh --athena ../athena --check
```

The default ONNX Runtime location is
`ATHENA/onnxruntime-linux-x64-gpu_cuda12-1.27.0`. If your working base uses a
different directory, pass it to the preview and installation commands:

```bash
./install-overlay.sh --athena /absolute/path/to/athena \
  --ort /absolute/path/to/onnxruntime --jobs 4
```

The normal build retains the reference CUDA options and targets `120a;86`.
For a different supported GPU architecture list, quote the semicolon-separated
value, for example `--cuda-arch '86'`. `--jobs` changes build parallelism only.
`--cpu` is an explicit CPU-only development build, not the reference GPU setup.
Both application build directories are rebuilt from scratch; the old directories
are moved into the printed backup. This prevents old objects surviving merely
because extracted source files have older timestamps. Reserve space for the new
builds alongside the retained backup.

The script prints a backup directory and its `restore.sh` command before copying.
Keep that directory. On any failed installation or check, stop and use the error
and rollback instructions before launching Athena.

### Vision projector

The base installer does not fetch the camera's multimodal projector. A normal
overlay installation downloads `models/mmproj-BF16.gguf` for the supplied
**Qwen3.5-397B-A17B** launcher from
[Unsloth's matching model repository](https://huggingface.co/unsloth/Qwen3.5-397B-A17B-GGUF/blob/da33c16fa4440f831149fcf53b98a22bc07785e5/mmproj-BF16.gguf).
The download is pinned to publisher revision
`da33c16fa4440f831149fcf53b98a22bc07785e5`, size **921,705,184 bytes**,
and SHA-256
`b3624272d7b9b49ffe6c6d0c592980bed6b026ce59cde11708bb230395c2a227`.

For an already installed overlay, add or verify just this model without copying
sources, changing the launcher or rebuilding:

```bash
./install-overlay.sh --athena ../athena --vision-model-only
```

A matching existing file is verified locally and reused without any network
request. Downloads resume from a hash-specific `.part` file and become visible
under the launcher's filename only after size and checksum verification. A
failed download leaves sources and builds unchanged; rerun the command to resume.
An existing mismatched projector is **never overwritten**: move it aside yourself
if it is corrupt, or keep it and use `--skip-vision-model` for a custom setup.
An oversized or full-sized corrupt partial must likewise be moved aside before
retrying; its exact path is printed. Model-directory/file symlinks are not followed.

For an offline, custom-model or deliberately non-vision installation, pass
`--skip-vision-model` to both install and `--check`. Other language-model variants
need their own matching projector; a shared `mmproj-BF16.gguf` filename does not
prove compatibility. Custom model paths and `--mmproj` settings remain your
responsibility. `--skip-build` and `--check-sources` remain source-only and never
download or verify model weights. All dry-run and check modes are network-free;
`--check` verifies this projector unless explicitly skipped. Downloading it does
not enable a camera, grant camera permission or change your existing launcher;
the launcher must actually pass `--mmproj` (as the supplied launcher does).

### Choose the launcher

The default preserves your existing root `launch-athena-397b.sh` and desktop
shortcut. The supplied launcher includes progress-aware shutdown supervision and
a speech protocol check. For a stock base, compare its model paths, context size,
offload settings and audio device choices with your working launcher before
installing it:

```bash
diff -u ../athena/launch-athena-397b.sh patches/launch-athena-397b.sh

./install-overlay.sh --athena ../athena --jobs 4 --launcher
./install-overlay.sh --athena ../athena --check --launcher
```

`diff` returning 1 means the files differ. `--launcher` backs up and replaces
the root launcher; without it, the supplied launcher is only staged under the
base's `patches/`. If you maintain a customized launcher, merge the supervision
and protocol preflight into your own copy while retaining your model/device
settings. Check it without `--launcher`, since that option requires an exact
match to the supplied file. A manual launcher edit needs its own backup.

### Enable the R26.1 runtime profile

The R26.1 feature family uses the environment switch `ATHENA_R26=1`. Set it in the
environment that actually launches Athena; installing the sources does not set
it persistently:

```bash
cd /absolute/path/to/athena
ATHENA_R26=1 ATHENA_INITIATIVE_POLICY=sustained ATHENA_DREAM_AFTER_S=420 \
ATHENA_MIND_DEBUG=1 ATHENA_FIELD_VERBOSITY=1.0 ATHENA_MIND_TRACE_S=2 \
GGML_CUDA_NO_PINNED=1 ATHENA_MPS=1 ./launch-athena-397b.sh
```

For a desktop launch, put these exports in your own launch wrapper or environment
setup and point the desktop entry at that wrapper. An export made in an unrelated
terminal does not reach an already running desktop session. Existing per-feature
`ATHENA_R26_*` settings can override the master-on setting; review any explicit
zeros in your environment. The startup `[r26]` line reports which feature groups
are enabled. Runtime feature switches do not replace an exact source/binary
rollback.

Keep your existing memory path and model files. For trace visibility, the supplied
launcher enables `ATHENA_MIND_DEBUG=1`; custom launchers must retain their own log
capture and debug configuration. Camera use and image retention continue to use
Athena's runtime permissions.

## What is installed

All destinations below are relative to the **stock Athena root**:

| Overlay source | Installed destination |
| --- | --- |
| `patches/talk-llama/` | `patches/talk-llama/` and `whisper.cpp/examples/talk-llama/`, recursively, including `mtmd/`. |
| `patches/whisper-common/` | `patches/whisper-common/` and the matching files in `whisper.cpp/examples/`. |
| `patches/orpheus/` | `patches/orpheus/` and `orpheus/`. Stock `orpheus/CMakeLists.txt` stays in place. |
| `patches/speak-daemon.sh` | `patches/speak-daemon.sh` and root `speak-daemon.sh`. |
| Other scripts in `patches/` | Matching paths under the base's `patches/`; the root launcher is replaced only with `--launcher`. |
| Pinned publisher projector (download) | `models/mmproj-BF16.gguf`; full installs only, unless `--skip-vision-model`. Also available through `--vision-model-only`. |

Stock `install.sh`, its desktop entries, `llama.cpp/`, model weights and personal
memory remain in place; the only added model is the missing vision projector.
The README, entry-point wrapper and changelog belong to
the separate overlay repository and are not copied over the base repository.
Keep that separate repository to run future checks or reinstallations.

**Do not rerun stock `install.sh` after applying the overlay.** Its
`cp patches/talk-llama/*` step is non-recursive and fails on `mtmd/`; its dependency
checkout and existing-binary checks can also discard or bypass overlay work.
Use `install-overlay.sh` for overlay rebuilds. To return to stock installation
management, restore the pre-overlay backup first. The stock installer itself
is preserved byte-for-byte.

## Source-only installation and custom builds

For a custom CMake configuration:

```bash
./install-overlay.sh --athena ../athena --skip-build
./install-overlay.sh --athena ../athena --check-sources
```

These commands verify sources only. They do not certify that installed binaries
contain R26.1. Preserve the existing `whisper.cpp/build` and `orpheus/build`
directories separately, configure fresh directories using your known working
settings, and rebuild all three targets:

```bash
cmake --build /absolute/path/to/athena/whisper.cpp/build \
  --target whisper-talk-llama athena-emotion-calibrate -j 4
cmake --build /absolute/path/to/athena/orpheus/build \
  --target orpheus-speak -j 4

./install-overlay.sh --athena /absolute/path/to/athena --check
```

The brain configuration needs `WHISPER_SDL2=ON` and your `ONNXRUNTIME_ROOT`.
Retain the installed `mtmd/` tree and vision build support, and compile the two
shared audio files together. The calibrator is available when ONNX Runtime is
configured. The speech application also needs `ONNXRUNTIME_ROOT`.

`--check` compares installed source bytes, checks executable timestamps and
shared-library resolution, runs the native speech session-protocol probe, and
verifies the default projector's size/checksum (unless `--skip-vision-model`).
It fails on missing or stale applications. Source timestamps and a protocol
probe cannot prove the provenance of arbitrary manually supplied binaries.

## Rollback and repeat installation

Stop Athena, then run the exact command printed by the installation you are
reverting:

```bash
bash /absolute/path/to/athena/athena-source-backup.XXXXXX/restore.sh
```

The restore script recovers the saved source files and, for a normal installer
build, both saved build directories. It preserves the replaced candidate under
that backup for inspection and rejects an accidental second restore of the same
backup. Restore successive installations in reverse order. Keep Athena and its
backup directories at their recorded locations until restoration is complete.

A source-only install leaves build directories untouched; if you subsequently
build manually, restore your separately saved build directories or rebuild the
restored sources. A manually edited launcher and personal-memory restoration
are separate operations. The script never silently restores personal memory.
The source rollback does not remove downloaded models or resumable partials.
A verified projector can remain in `models/` when returning to stock sources.

Reapplying the same overlay is supported. It recopies and verifies the same
sources, creates a new backup and, for a normal install, performs fresh builds.
Concurrent installer/check/restore operations against the same base are rejected.

## Validation scope

The projector installer passed **34 local regression tests** using a small
checksum-pinned download fixture and controlled compiler tools. These exercised
fresh/resumed downloads, corruption and HTTP failures, existing-file preservation,
read-only modes, source copying, build invocation and rollback, including tests
against the stock checkout's Git-tracked sources. A separate live test downloaded
the actual publisher projector, verified its checksum and confirmed network-free
reuse. These tests did not load a language model or exercise a physical camera/GPU;
the test tooling is not included in this repository.

The original packaging validation covered **22 tests** and 66 command executions.
It checked all **165 installed source destinations** (166 with `--launcher`)
against the pinned stock tree, preservation of stock installer/model/memory/
launcher files, repeat installation, source verification, path handling,
dependency rejection, concurrent-operation rejection and rollback. This includes
recovery when interruption occurs between moving a build and recording the move.
Build-command and failure-path tests use controlled compiler-tool fixtures;
they do not substitute for a CUDA build or a live-model test.

The R26.1 runtime source bytes retain the release's native behavior and linked
CPU integration validation, including the vision and non-vision brain,
calibrator and native speech application. This packaging environment does not
have the complete ONNX/CMake/CUDA toolchain for a fresh native application build.
On your fully installed Athena system, the normal installer performs that build
and its installation checks. Microphone, camera, model quality, GPU behavior and
audible prosody still require a live run on that system.

## License

<!-- ATHENA-NCRD-LICENSE-START -->
**Athena Consciousness is source-available for permitted noncommercial use.**
Covered original contributions use the
[Athena Noncommercial, Attribution and Responsible Deployment License, version 2.0](LICENSE).
Personal use, qualifying noncommercial research, updates, modification and free
redistribution are permitted under its conditions. Commercial use, including
internal business use, commercial R&D, paid services and monetized deployments,
is not licensed. There is no automatic commercial conversion or buyout.

**Original Athena Consciousness contributions by Igor Barshteyn.**
Third-party components belong to their respective authors.
Source: https://github.com/igorbarshteyn/athena-consciousness
Preserve this credit for Igor-authored Covered Material, applicable upstream
notices, and notices identifying Your changes. See [NOTICE](NOTICE).

The license prohibits deliberate mistreatment and requires responsible, lawful
deployment. It preserves personal control, ordinary shutdown, privacy deletion,
repair and proportionate noncommercial safety research. It does not require
agreement that Athena is conscious or give an AI authority to waive these terms.

**Read LICENSE Sections 12-14:** warranty disclaimers, liability exclusions and
limits, and a defense/indemnification obligation benefiting Igor Barshteyn and
other Protected Parties, subject to stated exclusions and mandatory law.
Contractual duties require legally effective acceptance. An ordinary guest does
not become an indemnitor merely by talking to someone else's deployment.

**Earlier valid licenses and third-party rights remain in force.** This license
does not retract older MIT permissions or relicense models, NVIDIA software or
upstream code. Read the [scope](LICENSING-SCOPE.md),
[transition](LICENSING-TRANSITION.md), [software notices](THIRD-PARTY-NOTICES.md),
[model licenses](MODEL-LICENSES.md), [deployment guide](DEPLOYMENT-COMPLIANCE.md),
[FAQ](LICENSE-FAQ.md), and [acceptance guidance](licensing/ACCEPTANCE-AND-RECORDS.md).
<!-- ATHENA-NCRD-LICENSE-END -->
