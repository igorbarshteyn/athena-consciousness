#!/usr/bin/env bash
# R26.1 overlay installer; see the repository README.md.
set -Eeuo pipefail

OVERLAY="$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")/.." && pwd)"
ATHENA=""; MODE=install; DO_BUILD=1; DO_LAUNCHER=0; ORT=""; JOBS=4; ALLOW_DRIFT=0
CPU=0; CUDA_ARCH='120a;86'; DRY_RUN=0; DO_VISION_MODEL=1
usage() {
    cat <<'HELP'
Athena R26.1 overlay for an already installed stock Athena checkout
Usage: ./install-overlay.sh --athena DIR [options]

  --athena DIR       Existing Athena root; required.
  --ort DIR          ONNX Runtime root; default:
                     ATHENA/onnxruntime-linux-x64-gpu_cuda12-1.27.0
  --jobs N           Parallel build jobs (default: 4).
  --cuda-arch LIST   CMake CUDA architectures (default: 120a;86).
  --cpu             CPU-only development/verification build.
  --launcher        Also replace the root launcher; review its settings first.
  --vision-model-only
                    Install/verify the matching Qwen3.5-397B vision projector
                    only; do not copy sources or rebuild applications.
  --skip-vision-model
                    Do not download/check the default projector (offline,
                    custom-model or deliberately non-vision installations).
  --dry-run         Preflight and list destinations; change nothing.
  --skip-build      Copy and verify sources only; NOT ready to launch.
  --check-sources   Verify installed sources only; change nothing.
  --check           Verify sources, binaries, speech protocol and projector.
  --allow-drift     Permit an untested dependency; compatibility is not assured.
  -h, --help        Show this help.

Stop Athena before installing or restoring. The installer preserves a source
backup and, for a full build, both old build directories. On failure, use the
printed restore.sh command. Stock install.sh is never invoked or overwritten.
Full installs fetch the missing, checksum-pinned vision projector (~922 MB).
Existing models are never overwritten. Source-only/check/dry-run modes never
download. Personal memory, desktop entries and the llama.cpp server are untouched.
HELP
}
while [ $# -gt 0 ]; do
    case "$1" in
        --athena|--ort|--jobs|--cuda-arch)
            [ $# -ge 2 ] && [ -n "$2" ] && [[ "$2" != --* ]] || { echo "install-consciousness: $1 needs a value" >&2; exit 2; } ;;
    esac
    case "$1" in
        --athena) ATHENA="$2"; shift 2 ;;
        --check|--check-sources|--vision-model-only)
            [ "$MODE" = install ] || { echo 'choose one operation mode' >&2; exit 2; }
            MODE="${1#--}"; shift ;;
        --skip-vision-model) DO_VISION_MODEL=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --cpu) CPU=1; shift ;;
        --cuda-arch) CUDA_ARCH="$2"; shift 2 ;;
        --skip-build) DO_BUILD=0; shift ;;
        --launcher) DO_LAUNCHER=1; shift ;;
        --ort) ORT="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --allow-drift) ALLOW_DRIFT=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install-consciousness: unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$ATHENA" ] || { echo "install-consciousness: --athena /abs/path/to/athena is required" >&2; exit 2; }
[[ "$JOBS" =~ ^[1-9][0-9]{0,3}$ ]] || { echo "install-consciousness: --jobs needs a positive integer" >&2; exit 2; }
[[ "$CUDA_ARCH" =~ ^[A-Za-z0-9_.+-]+(\;[A-Za-z0-9_.+-]+)*$ ]] || { echo 'invalid --cuda-arch list' >&2; exit 2; }
if [[ "$MODE" == check* ]] && { [ "$DO_BUILD" = 0 ] || [ "$DRY_RUN" = 1 ]; }; then
    echo '--check/--check-sources cannot be combined with --skip-build or --dry-run' >&2; exit 2
fi
if [ "$MODE" = vision-model-only ] && { [ "$DO_VISION_MODEL" = 0 ] || [ "$DO_BUILD" = 0 ] || [ "$DO_LAUNCHER" = 1 ]; }; then
    echo '--vision-model-only cannot be combined with --skip-vision-model, --skip-build or --launcher' >&2; exit 2
fi
ATHENA="$(cd -- "$ATHENA" && pwd -P)"
[[ "$OVERLAY/" != "$ATHENA/"* && "$ATHENA/" != "$OVERLAY/"* ]] || { echo "install-consciousness: use separate, non-nested overlay and Athena directories" >&2; exit 2; }
ORT="${ORT:-$ATHENA/onnxruntime-linux-x64-gpu_cuda12-1.27.0}"

fail_n=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*" >&2; fail_n=$((fail_n+1)); }
warn() { printf '  warn  %s\n' "$*" >&2; }
die()  { printf 'install-consciousness: %s\n' "$*" >&2; exit 1; }

# The supplied launcher uses this projector with Qwen3.5-397B-A17B. Pin both
# the immutable publisher revision and its Git LFS SHA-256, not mutable main.
# Do not infer compatibility from the generic filename or GGUF magic alone.
vision_model() (
    action="$1"
    revision=da33c16fa4440f831149fcf53b98a22bc07785e5
    expected_sha=b3624272d7b9b49ffe6c6d0c592980bed6b026ce59cde11708bb230395c2a227
    expected_size=921705184
    url="https://huggingface.co/unsloth/Qwen3.5-397B-A17B-GGUF/resolve/$revision/mmproj-BF16.gguf"
    dest="$ATHENA/models/mmproj-BF16.gguf"
    # A hash-specific partial cannot accidentally resume another release.
    partial="$dest.${expected_sha:0:12}.part"
    [ ! -L "$ATHENA/models" ] || die "models directory is a symlink; use --skip-vision-model and manage your projector separately"
    [ ! -e "$ATHENA/models" ] || [ -d "$ATHENA/models" ] || die "models is not a directory"
    for path in "$dest" "$partial"; do
        [ ! -L "$path" ] || die "projector path is a symlink (left untouched): $path"
        [ ! -e "$path" ] || [ -f "$path" ] || die "projector path is not a regular file: $path"
    done
    command -v sha256sum >/dev/null || die "sha256sum is required to verify the vision projector"
    verify_projector() {
        [ "$(stat -c %s -- "$1")" = "$expected_size" ] || return 1
        # Read on stdin so unusual checkout names cannot escape checksum output.
        digest="$(sha256sum < "$1")" || return 1
        [ "${digest%% *}" = "$expected_sha" ]
    }
    if [ -f "$dest" ]; then
        verify_projector "$dest" || die "existing projector does not match the pinned Qwen3.5-397B BF16 model; left untouched: $dest. Move it aside yourself to fetch this model, or use --skip-vision-model for a custom setup"
        ok "vision projector present; size and SHA-256 verified (no download)"
        exit 0
    fi
    [ "$action" != check ] || die "vision projector missing: $dest; run --vision-model-only (or --skip-vision-model for a custom/non-vision setup)"
    have=0
    if [ -f "$partial" ]; then
        [ "$(stat -c %h -- "$partial")" = 1 ] || die "refusing to write a hard-linked projector partial: $partial"
        have="$(stat -c %s -- "$partial")"
        [ "$have" -le "$expected_size" ] || die "projector partial is oversized; left untouched: $partial. Move it aside before retrying"
    fi
    if [ "$action" = preview ]; then
        printf '  vision  %s -> %s (%s bytes; resume from %s; SHA-256 verified before use)\n' "$url" "$dest" "$expected_size" "$have"
        exit 0
    fi
    if [ "$have" -lt "$expected_size" ]; then
        command -v curl >/dev/null || die "curl is required to download the missing vision projector (or use --skip-vision-model)"
        mkdir -p -- "$ATHENA/models"
        printf '  vision  downloading/resuming Qwen3.5-397B BF16 projector (%s / %s bytes)\n' "$have" "$expected_size"
        # Never expose an incomplete download under the filename the launcher
        # loads. TLS, HTTP failure handling and the pinned digest reject error
        # pages, truncation and wrong-model responses. Re-run after interruption.
        curl -q --fail --location --proto '=https' --proto-redir '=https' \
            --connect-timeout 30 --speed-limit 1024 --speed-time 60 \
            --retry 3 --retry-delay 2 --continue-at - --output "$partial" "$url" \
            || die "vision download failed; sources/builds are unchanged. Partial retained at $partial; re-run to resume"
    fi
    verify_projector "$partial" || die "projector size/SHA-256 verification failed; not installed. Partial retained at $partial; re-run to resume if incomplete, or move it aside if corrupt"
    # Atomic, no-clobber publication on the same filesystem. Even a file
    # created concurrently by an external process is never overwritten.
    chmod 644 -- "$partial"
    ln -T -- "$partial" "$dest" || die "cannot publish projector without overwriting $dest; verified partial retained at $partial"
    rm -- "$partial"
    ok "vision projector installed; size and SHA-256 verified: $dest"
)

# Serialize installs/checks/rollbacks without leaving a file in read-only modes.
command -v flock >/dev/null || die "flock is required"
exec {install_lock}<"$ATHENA"
flock -n "$install_lock" || die "another overlay operation is using $ATHENA"

# ── 0. what we are looking at ────────────────────────────────────────────────
[ -f "$ATHENA/install.sh" ] && [ -d "$ATHENA/patches/talk-llama" ] && [ -d "$ATHENA/orpheus" ] \
    || die "$ATHENA does not look like an Athena checkout (install.sh, patches/talk-llama, orpheus/)"
[ -d "$ATHENA/whisper.cpp/examples/talk-llama" ] \
    || die "$ATHENA/whisper.cpp is not cloned yet; finish the stock Athena installation first"
[ -f "$ATHENA/whisper.cpp/CMakeLists.txt" ] && [ -f "$ATHENA/orpheus/CMakeLists.txt" ] \
    || die "base build definitions are missing; finish the stock Athena installation first"
if [ "$MODE" = vision-model-only ]; then
    if [ "$DRY_RUN" = 1 ]; then vision_model preview; else vision_model install; fi
    exit 0
fi
[ ! -L "$OVERLAY/patches" ] || die "overlay patches must be a real directory"
[ -f "$OVERLAY/patches/talk-llama/talk-llama.cpp" ] && [ -f "$OVERLAY/patches/whisper-common/common-sdl.h" ] \
    || die "$OVERLAY does not look like the extracted overlay"
# The tested whisper.cpp commit is stock install.sh's single source of truth.
PIN="$(sed -n 's/^WHISPER_PIN="\([0-9a-f]\{40\}\)".*/\1/p' "$ATHENA/install.sh" | head -1)"
TESTED_PIN=afa2ea544fb4b0448916b4a31ecd33c8685bd482
if [ "$PIN" != "$TESTED_PIN" ]; then
    [ "$ALLOW_DRIFT" = 1 ] || die "stock installer has an unknown/untested Whisper pin (${PIN:-unknown}); nothing copied"
    warn "stock installer pin is not the tested R26.1 dependency (--allow-drift)"
fi
PIN="$TESTED_PIN"
echo "install-consciousness: overlay $OVERLAY"
echo "                       athena  $ATHENA  (mode: $MODE)"

# ── 1. the whisper tree must sit at the pin BEFORE anything is copied ────────
# Stock install.sh, seeing HEAD != pin, runs `git checkout -f` — which throws
# away the overlay's tracked examples/common-sdl.{h,cpp}. Say so now, while a
# `git -C whisper.cpp checkout <pin>` still costs nothing.
head="$(git -C "$ATHENA/whisper.cpp" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$head" = "$PIN" ]; then ok "whisper.cpp at the tested pin ${PIN:0:8}"
elif [ "$ALLOW_DRIFT" = 1 ]; then warn "whisper.cpp HEAD ${head:0:8} is not the pin ${PIN:0:8} (--allow-drift); never run stock install.sh on this tree — it would checkout -f and revert common-sdl.*"
else bad "whisper.cpp HEAD ${head:0:8} is not the tested pin ${PIN:0:8}: run  git -C '$ATHENA/whisper.cpp' checkout $PIN  first (or --allow-drift)"; fi
if [ "$MODE" = install ] && [ "$fail_n" != 0 ]; then
    die "nothing copied — put whisper.cpp at the pin first (stock install.sh would 'checkout -f' this tree and revert the overlay's common-sdl.*), then re-run"
fi

# Every copied file is checked against the extracted release, including the
# full mtmd subtree and both independent build trees. No timestamp proves that
# two source files are equal. All paths remain individual, quoted arguments.
SOURCE_ROOTS=(patches/talk-llama patches/whisper-common patches/orpheus patches/orpheus)
DEST_ROOTS=(patches/talk-llama patches/whisper-common orpheus patches/orpheus)
SOURCE_ROOTS+=(patches/talk-llama patches/whisper-common)
DEST_ROOTS+=(whisper.cpp/examples/talk-llama whisper.cpp/examples)
SOURCE_FILES=(patches/speak-daemon.sh patches/speak-daemon.sh patches/launch-athena-397b.sh patches/athena-vision-check.sh patches/install-consciousness.sh)
DEST_FILES=(speak-daemon.sh patches/speak-daemon.sh patches/launch-athena-397b.sh patches/athena-vision-check.sh patches/install-consciousness.sh)
if [ "$DO_LAUNCHER" = 1 ]; then
    SOURCE_FILES+=(patches/launch-athena-397b.sh); DEST_FILES+=(launch-athena-397b.sh)
fi
# Refuse destination symlinks rather than following them outside the checkout.
# A launcher/wrapper symlink as the final file is backed up and replaced safely.
check_parent() {
    local path="$1" parent="$ATHENA" part
    while [[ "$path" == */* ]]; do
        part="${path%%/*}"; path="${path#*/}"; parent="$parent/$part"
        [ ! -L "$parent" ] || die "destination directory is a symlink: $parent"
        [ ! -e "$parent" ] || [ -d "$parent" ] || die "destination parent is not a directory: $parent"
    done
}
for i in "${!SOURCE_ROOTS[@]}"; do
    [ -d "$OVERLAY/${SOURCE_ROOTS[$i]}" ] || die "missing overlay directory ${SOURCE_ROOTS[$i]}"
    check_parent "${DEST_ROOTS[$i]}/placeholder"
done
for dest in "${DEST_FILES[@]}"; do check_parent "$dest"; done
# Check every nested destination BEFORE backup/copy, including mtmd/vendor.
preflight_file() {
    local source="$1" dest="$2"
    [ -f "$source" ] && [ ! -L "$source" ] || die "missing/nonregular overlay file: $source"
    check_parent "$dest"
    [ ! -e "$ATHENA/$dest" ] || [ -f "$ATHENA/$dest" ] || [ -L "$ATHENA/$dest" ] \
        || die "destination is not a file: $dest"
}
for i in "${!SOURCE_ROOTS[@]}"; do
    source_root="$OVERLAY/${SOURCE_ROOTS[$i]}"
    [ ! -L "$source_root" ] && [ -z "$(find "$source_root" -mindepth 1 ! -type f ! -type d -print -quit)" ] \
        || die "overlay tree contains nonregular files: $source_root"
    while IFS= read -r -d '' source; do
        relative="${source#"$source_root/"}"
        preflight_file "$source" "${DEST_ROOTS[$i]}/$relative"
    done < <(find "$source_root" -type f -print0)
done
for i in "${!SOURCE_FILES[@]}"; do preflight_file "$OVERLAY/${SOURCE_FILES[$i]}" "${DEST_FILES[$i]}"; done
cmp -s "$OVERLAY/patches/talk-llama/athena_tts_wire.h" "$OVERLAY/patches/orpheus/athena_tts_wire.h" \
    || die "brain and speech wire headers differ"
for file in talk-llama/CMakeLists.txt talk-llama/athena_r26.h \
    talk-llama/athena-emotion-calibrate.cpp talk-llama/mtmd/mtmd.cpp \
    whisper-common/common-sdl.cpp orpheus/orpheus-speak.cpp orpheus/athena_prosody.h; do
    [ -f "$OVERLAY/patches/$file" ] || die "incomplete overlay: patches/$file"
done

if [ "$MODE" = install ] && [ "$fail_n" = 0 ]; then
    if [ "$DO_BUILD" = 1 ]; then
        ORT="$(cd -- "$ORT" && pwd -P)" || die "ONNX Runtime directory missing (--ort DIR); nothing copied"
        [ -f "$ORT/include/onnxruntime_cxx_api.h" ] && [ -f "$ORT/lib/libonnxruntime.so" ] \
            || die "ONNX Runtime headers/library missing at $ORT (--ort DIR); nothing copied"
        for cmd in ldd timeout; do command -v "$cmd" >/dev/null || die "$cmd is required; nothing copied"; done
        check_parent whisper.cpp/build/placeholder
        check_parent orpheus/build/placeholder
        command -v cmake >/dev/null || die "cmake is required; nothing copied"
    fi
    if [ "$DO_LAUNCHER" = 1 ]; then
        command -v setsid >/dev/null || die "the supervised launcher needs setsid; nothing copied"
    fi
    if [ "$DRY_RUN" = 1 ]; then
        if [ "$DO_BUILD" = 1 ] && [ "$DO_VISION_MODEL" = 1 ]; then vision_model preview; fi
        for i in "${!SOURCE_ROOTS[@]}"; do printf '  %s/ -> %s/ (recursive)\n' "${SOURCE_ROOTS[$i]}" "${DEST_ROOTS[$i]}"; done
        for i in "${!SOURCE_FILES[@]}"; do printf '  %s -> %s\n' "${SOURCE_FILES[$i]}" "${DEST_FILES[$i]}"; done
        printf 'Preflight passed; rebuild=%s launcher=%s CPU=%s. Nothing changed.\n' "$DO_BUILD" "$DO_LAUNCHER" "$CPU"
        exit 0
    fi
    # Fetch/verify before touching sources or builds. Model weights are not
    # part of the source rollback; a verified new projector can be kept.
    if [ "$DO_BUILD" = 1 ] && [ "$DO_VISION_MODEL" = 1 ]; then vision_model install; fi
    cd "$ATHENA"
    # Save the ACTUAL build sources too: local changes in examples/talk-llama
    # need not match patches/. Record absent paths so rollback removes additions.
    BACKUP="$(mktemp -d "$ATHENA/athena-source-backup.XXXXXX")"
    restore_paths=(patches whisper.cpp/examples/talk-llama
        whisper.cpp/examples/common-sdl.h whisper.cpp/examples/common-sdl.cpp
        orpheus/orpheus-speak.cpp orpheus/athena_prosody.h orpheus/athena_tts_wire.h speak-daemon.sh)
    [ "$DO_LAUNCHER" = 0 ] || restore_paths+=(launch-athena-397b.sh)
    existing=()
    for f in "${restore_paths[@]}"; do
        if [ -e "$f" ] || [ -L "$f" ]; then existing+=("$f"); fi
    done
    tar -cpf "$BACKUP/sources.tar" -- "${existing[@]}"
    # A self-contained rollback also preserves the failed/replaced candidate.
    # The old build directories are moved, not copied, before a fresh build.
    # Sources and saved binaries are restored as one explicit operation.
    {
        printf '#!/usr/bin/env bash\nset -Eeuo pipefail\n'
        printf 'ATHENA=%q\nBACKUP=%q\n' "$ATHENA" "$BACKUP"
        printf 'paths=('; printf '%q ' "${restore_paths[@]}"; printf ')\n'
        cat <<'ROLLBACK'
[ ! -e "$BACKUP/restored" ] || { echo 'This backup has already been restored.' >&2; exit 1; }
exec {restore_lock}<"$ATHENA"
flock -n "$restore_lock" || { echo 'Another overlay operation is active.' >&2; exit 1; }
cd -- "$ATHENA"
REPLACED="$(mktemp -d "$BACKUP/replaced.XXXXXX")"
for path in "${paths[@]}"; do
    if [ -e "$path" ] || [ -L "$path" ]; then
        mkdir -p "$REPLACED/$(dirname -- "$path")"
        mv -- "$path" "$REPLACED/$path"
    fi
done
tar -xpf "$BACKUP/sources.tar" -C "$ATHENA"
for spec in whisper.cpp:whisper-build orpheus:orpheus-build; do
    root="${spec%:*}"; saved="${spec#*:}"
    # The saved directory also proves a move if interruption preceded its marker.
    if [ -e "$BACKUP/$saved.was-moved" ] || [ -d "$BACKUP/$saved" ]; then
        if [ -e "$root/build" ] || [ -L "$root/build" ]; then mv -- "$root/build" "$REPLACED/$saved"; fi
        if [ -d "$BACKUP/$saved" ]; then mv -- "$BACKUP/$saved" "$root/build"; fi
        rm -f -- "$BACKUP/$saved.was-moved"
    fi
done
touch "$BACKUP/restored"
printf 'Restored source backup. Replaced files preserved in %s\n' "$REPLACED"
ROLLBACK
    } > "$BACKUP/restore.sh"
    chmod u+x "$BACKUP/restore.sh"
    trap 'status=$?; if [ "$status" != 0 ]; then printf "Install failed; do not launch. Rollback: bash %q\n" "$BACKUP/restore.sh" >&2; fi' EXIT
    ok "source backup: $BACKUP (rollback: bash '$BACKUP/restore.sh')"
    for i in "${!SOURCE_ROOTS[@]}"; do
        mkdir -p "${DEST_ROOTS[$i]}"
        # Replace a final symlink rather than copying through it.
        while IFS= read -r -d '' source; do
            relative="${source#"$OVERLAY/${SOURCE_ROOTS[$i]}/"}"
            dest="${DEST_ROOTS[$i]}/$relative"
            check_parent "$dest"
            [ ! -L "$dest" ] || rm -- "$dest"
        done < <(find "$OVERLAY/${SOURCE_ROOTS[$i]}" -type f -print0)
        cp -a --remove-destination "$OVERLAY/${SOURCE_ROOTS[$i]}/." "${DEST_ROOTS[$i]}/"
    done
    for i in "${!SOURCE_FILES[@]}"; do
        dest="${DEST_FILES[$i]}"
        [ ! -L "$dest" ] || rm -- "$dest"
        cp -a --remove-destination "$OVERLAY/${SOURCE_FILES[$i]}" "$dest"
        chmod u+x "$dest"
    done
    ok "copied complete brain, mtmd, shared audio, daemon, wrapper and diagnostics"
fi

cd "$ATHENA"
checked=0
check_source() {
    local source="$1" dest="$2"
    if [ -f "$dest" ] && [ ! -L "$dest" ] && cmp -s "$source" "$dest"; then
        checked=$((checked+1))
    else bad "$dest is missing or differs from the extracted release"; fi
}
for i in "${!SOURCE_ROOTS[@]}"; do
    while IFS= read -r -d '' source; do
        relative="${source#"$OVERLAY/${SOURCE_ROOTS[$i]}/"}"
        check_source "$source" "${DEST_ROOTS[$i]}/$relative"
    done < <(find "$OVERLAY/${SOURCE_ROOTS[$i]}" -type f -print0)
done
for i in "${!SOURCE_FILES[@]}"; do check_source "$OVERLAY/${SOURCE_FILES[$i]}" "${DEST_FILES[$i]}"; done
ok "$checked installed source destinations match the extracted release byte-for-byte"
if [ "$fail_n" != 0 ]; then die "$fail_n source/pin check(s) failed; do not build or launch"; fi
if [ "$MODE" = check-sources ] || { [ "$MODE" = install ] && [ "$DO_BUILD" = 0 ]; }; then
    echo "install-consciousness: SOURCE COPY VERIFIED — binaries were not rebuilt or validated."
    echo "                       Use fresh build directories: preserved source mtimes can hide stale objects."
    echo "                       Rebuild both applications before launching; backup: ${BACKUP:-not applicable}"
    exit 0
fi

# ── 5. the builds (the guide's two blocks) ───────────────────────────────────
if [ "$MODE" = install ] && [ "$DO_BUILD" = 1 ] && [ "$fail_n" = 0 ]; then
    for spec in whisper.cpp:whisper-build orpheus:orpheus-build; do
        root="${spec%:*}"; saved="${spec#*:}"
        if [ -e "$root/build" ] || [ -L "$root/build" ]; then mv -- "$root/build" "$BACKUP/$saved"; fi
        touch "$BACKUP/$saved.was-moved"
    done
    echo "install-consciousness: building whisper-talk-llama + athena-emotion-calibrate (-j $JOBS)"
    brain_args=(-DWHISPER_SDL2=ON "-DONNXRUNTIME_ROOT=$ORT" -DCMAKE_BUILD_TYPE=Release)
    if [ "$CPU" = 1 ]; then
        brain_args+=(-DGGML_CUDA=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF)
    else
        brain_args+=(-DGGML_CUDA=ON "-DCMAKE_CUDA_ARCHITECTURES=$CUDA_ARCH"
            -DGGML_CUDA_FORCE_CUBLAS=OFF -DGGML_CUDA_FA_ALL_QUANTS=ON -DGGML_CUDA_F16=ON
            -DGGML_SCHED_MAX_COPIES=1 -DGGML_NATIVE=ON -DGGML_LTO=ON -DGGML_CUDA_GRAPHS=ON)
    fi
    cmake -S whisper.cpp -B whisper.cpp/build "${brain_args[@]}"
    cmake --build whisper.cpp/build -j "$JOBS" --target whisper-talk-llama athena-emotion-calibrate
    echo "install-consciousness: building orpheus-speak (-j $JOBS)"
    cmake -S orpheus -B orpheus/build -DONNXRUNTIME_ROOT="$ORT" -DCMAKE_BUILD_TYPE=Release
    cmake --build orpheus/build -j "$JOBS" --target orpheus-speak
fi

# ── 6. the verdict — what stock integrity() cannot see ───────────────────────
# A binary is stale when any source it is built from is newer than it. Stock
# install.sh's bin_ok is `-x` plus ldd; that is the false green.
for cmd in ldd timeout; do command -v "$cmd" >/dev/null || die "$cmd is required for --check"; done
newest() { find "$@" -type f -newer "$BIN" -print -quit; }
for spec in "whisper.cpp/build/bin/whisper-talk-llama whisper.cpp/examples/talk-llama whisper.cpp/examples/common-sdl.h whisper.cpp/examples/common-sdl.cpp" \
            "whisper.cpp/build/bin/athena-emotion-calibrate whisper.cpp/examples/talk-llama whisper.cpp/examples/common-sdl.h whisper.cpp/examples/common-sdl.cpp" \
            "orpheus/build/orpheus-speak orpheus/orpheus-speak.cpp orpheus/athena_prosody.h orpheus/athena_tts_wire.h orpheus/CMakeLists.txt"; do
    set -- $spec; BIN="$1"; shift
    if [ ! -x "$BIN" ] || [ -L "$BIN" ]; then bad "$BIN missing — not built"; continue; fi
    linkage="$(ldd "$BIN" 2>&1)" || { bad "$BIN linkage check failed: $linkage"; continue; }
    if [[ "$linkage" == *'not found'* ]]; then bad "$BIN has an unresolved shared library (ldd)"; continue; fi
    stale="$(newest "$@")"
    if [ -n "$stale" ]; then bad "$BIN is OLDER than its source $stale — rebuild it"; else ok "$BIN newer than every source it is built from"; fi
done
if [ -x orpheus/build/orpheus-speak ]; then
    if timeout --kill-after=2s 10s orpheus/build/orpheus-speak --trace-wrapper >/dev/null 2>&1; then ok "orpheus-speak session protocol (--trace-wrapper probe)"
    else bad "orpheus-speak protocol probe failed or timed out — rebuild the matching daemon"; fi
fi
[ -x ./speak-daemon.sh ] && grep -qs -- '--trace-wrapper' ./speak-daemon.sh && ok "speak-daemon.sh matches the session protocol" || bad "speak-daemon.sh lacks the session protocol"
if [ -f ./launch-athena-397b.sh ]; then
    if grep -qs 'ATHENA_SHUTDOWN_WAIT_PROGRESS' ./launch-athena-397b.sh; then ok "launcher: progress-aware shutdown supervision is present"
    elif grep -qs 'ATHENA_LAUNCH_SUPERVISE' ./launch-athena-397b.sh; then warn "launcher: shutdown has a fixed limit; review the supplied launcher supervision (--launcher)"
    else warn "launcher: stock foreground supervision; review the supplied launcher before live use"; fi
fi
if [ "$MODE" = check ] && [ "$DO_VISION_MODEL" = 1 ]; then
    vision_model check || bad "vision projector check failed"
elif [ "$DO_VISION_MODEL" = 0 ]; then
    warn "default vision projector not checked (--skip-vision-model); verify your model/launcher separately"
fi

echo
if [ "$fail_n" = 0 ]; then
    echo "install-consciousness: OK — release sources, executable timestamps, linkage and protocol probe passed."
    echo "                       Timestamp checks cannot prove a manually built binary's source provenance."
    echo "                       Do not run stock install.sh after this overlay. Enable ATHENA_R26=1; see README.md."
    exit 0
else
    echo "install-consciousness: $fail_n check(s) FAILED — do not launch. See the FAIL lines above." >&2
    exit 1
fi
