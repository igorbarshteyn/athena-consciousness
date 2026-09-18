#!/usr/bin/env bash
# launch-athena.sh — One-click launcher for the Athena voice assistant
#
# Starts all three processes (llama-server, orpheus-speak, talk-llama),
# waits for Ctrl+C, then shuts everything down and cleans up temp files.
#
# Suitable for launching from a desktop shortcut.
#
# Hardware: ThinkPad P16 Gen 3
#   CPU:  Intel Core Ultra 9 285HX (8P + 16E = 24 cores / 24 threads — no SMT)
#   RAM:  192 GB DDR5-4000
#   GPU:  NVIDIA RTX PRO 5000 Blackwell Laptop 24 GB GDDR7 (896 GB/s)
#   LLM:  Qwen3.5-397B-A17B UD-Q3_K_XL (routed experts on CPU via --cpu-moe)
#
# r24.6 (ATHENA-R246 fix plan, launcher-side work orders — shipped as a separate
# diff from the code patch, per plan §7.5):
#   WO-53  per-writer log streams: orpheus-speak -> orpheus-speak.log, talk-llama
#          stderr -> athena-diag.log, each through its own `ts`; stdout (her
#          speech) stays on the terminal / athena.log pipe untouched. Fixes the
#          2,028 glued records of S19. --silero-debug deleted (84.5 % of the log).
#   WO-48  orpheus-speak now gets --control (prosody/breath control file) — the
#          r21-B voice feature was inert without it.
#   WO-51  --endpoint-long-ms 800 -> 1100 (real headroom for the "not done yet"
#          branch; short stays 800, so nothing can fire earlier than before).
#   WO-65  --personality-reflect-every 1 demo flag reverted (DD5).
#   WO-66  emotion block comment regenerated to match the exports; MIN_HAPPY /
#          MIN_ANGRY 0.80 -> 0.65 (WO-03); the four gating knobs added; the
#          ctx-size and sampling comments now document what is actually passed.
#
# r24.7 (ATHENA-R247 fix plan, launcher-side):
#   WO-95  DD18 — request what the camera delivers: --camera-res 640x480 (the
#          rig's camera fell back from 1920x1080 to 640x480 on 4/4 S20
#          captures; request now == reality, see the camera block below), and
#          the S20-measured budgets: --look-max 10 -> 15, --recall-max (new on
#          the command line) 3 -> 4.
#
# r24.12 (ATHENA S22 fix plan, launcher-side):
#   WO-V1  DD22 — DD18 withdrawn: --camera-res 640x480 -> 1920x1080 (the
#          fallback was ffmpeg's raw-format preference, not the camera; see
#          the camera block below).
#   WO-V2  --vision-work-edge 800, --vision-acuity-edge 1920,
#          --vision-acuity-mode scale, --image-min-tokens 2040 (the working
#          copy, the acuity look and the acuity-only hedge; camera block).
#   §E.3   (review) the two documentation deliverables that were missed:
#          ATHENA_MIND_DEBUG=1 is exported beside ATHENA_EMOTION_DEBUG (the
#          S23 grep sheet's mind-gated lines are absent without it), and the
#          sampling block now records that the r14 DRY chain is in force BY
#          DEFAULT and is what WO-I7's spacing guard belts.
#          Also: the CONTEXT paragraph said --ctx-size 40000 above an
#          invocation passing 80000 — reconciled to 80000 (decision 10).
#
# ─────────────────────────────────────────────────────────────────────────────

set -e
set -E   # so ERR traps are inherited by shell functions
# ── r24.17 launcher supervision policy ──────────────────────────────────────
# The former foreground command left PID_TALK empty and delayed launcher-only
# TERM until the brain exited. Blindly backgrounding it also sends INT+TERM on
# terminal Ctrl+C: the brain intentionally force-exits on its second signal.
# Give the tracked brain its own session; the launcher sends one ordered stop.
# Linux setsid ships with util-linux, as does the already required taskset.
# Literal zero restores the supplied launcher's foreground/trap mechanism.
ATHENA_LAUNCH_SUPERVISE="${ATHENA_LAUNCH_SUPERVISE:-1}"
if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
    command -v setsid >/dev/null 2>&1 || {
        echo "[launch-athena] setsid is required for supervised launch (util-linux)." >&2
        exit 1
    }
fi
# ── end r24.17 launcher supervision policy ──────────────────────────────────
trap 'echo "[launch-athena] ERROR at line $LINENO: \"$BASH_COMMAND\" exited with $?" >&2' ERR

# ── Paths (auto-derived — the repo is relocatable) ────────────────────────────
# ATHENA_DIR is the directory holding this script, resolved THROUGH any symlink and
# independent of the caller's working directory — so the .desktop launcher, a symlink
# in ~/bin, cron, and a plain `./launch-athena-397b.sh` all resolve the same repo.
# Everything below is relative to it. Export ATHENA_DIR before launch only if the
# script lives apart from the models/binaries.
ATHENA_DIR="${ATHENA_DIR:-$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)}"
export ATHENA_DIR   # children (speak-daemon.sh, the monitor) inherit it

LLAMA_SERVER="$ATHENA_DIR/llama.cpp/build/bin/llama-server"
ORPHEUS_SPEAK="$ATHENA_DIR/orpheus/build/orpheus-speak"
TALK_LLAMA="$ATHENA_DIR/whisper.cpp/build/bin/whisper-talk-llama"

ORPHEUS_MODEL="$ATHENA_DIR/models/orpheus-3b-0.1-ft-UD-Q4_K_XL.gguf"
SNAC_MODEL="$ATHENA_DIR/orpheus/snac24_dynamic_fp16.onnx"
QWEN_MODEL="$ATHENA_DIR/models/Qwen3.5-397B-A17B-UD-Q3_K_XL-00001-of-00005.gguf"
QWEN_SHARDS=5   # split GGUF: shards 00001..00005 must all be present
WHISPER_MODEL="$ATHENA_DIR/models/ggml-small.en.bin"
VAD_MODEL="$ATHENA_DIR/models/ggml-silero-v6.2.0.bin"   # Silero streaming end-of-turn VAD (./models/download-vad-model.sh silero-v6.2.0)
SPEAK_DAEMON="$ATHENA_DIR/speak-daemon.sh"

# emotion2vec speech-emotion tagging (ONNX, runs inline in talk-llama on GPU 0).
# Adjust the path to wherever you placed the exported model. Comment the export
# (or remove the file) to disable; set ATHENA_EMOTION_CPU=1 to force the CPU EP.
EMOTION_MODEL="$ATHENA_DIR/models/emotion2vec_plus_large.onnx"
[ -f "$EMOTION_MODEL" ] && export ATHENA_EMOTION_ONNX="$EMOTION_MODEL"
#export ATHENA_EMOTION_CPU=1          # <-- add this line to run emotion detection on CPU
export ATHENA_EMOTION_DEBUG=1
# r24.12 (§E.3, review): the [mind/…] census the S23 grep sheet reads. Several
# sheet lines are mind_debug-gated and are simply ABSENT from athena-diag.log
# without this — `[vision] his words ask her to read …` and `[vision] invited
# look committed` (the WO-V2 acuity arms) and `[mind/fok] arm/settle` (WO-M16c).
# The unconditional ones (`[vision] her own words committed a look`,
# `[vision] album: request refused`) print either way. Set it here rather than
# in the sheet so a session recorded for review is always readable.
export ATHENA_MIND_DEBUG=1

# ── Emotion profile ───────────────────────────────────────────────────────────
# Two floor sets, selected by ATHENA_EMOTION_CALIBRATION (default = production).
#
# COMMENT REGENERATED r24.6 (WO-66, resolves S19 I3): the 35-line description
# that used to sit here was a frozen artefact of an older calibration pass — it
# said negative-collapse was ON, happy=0.50, sad=0.60, "surprised never
# exceeded 0.008" — none of which matched the exports eight lines below it.
# This text documents what is actually exported.
#
# PRODUCTION (default):
#   • COLLAPSE_NEGATIVE=0 — negative-collapse is OFF: emissions are per-class
#     ([emotion: sad], never [emotion: negative]). BOTH profiles set 0, so the
#     CALIBRATION switch below only swaps FLOOR SETS — it does not toggle
#     collapse, whatever older notes claimed.
#   • The single most consequential setting in this file is the RETIREMENT of
#     surprised / fearful / disgusted: a floor of 1.01 means those classes can
#     never emit (their true and false readings overlap on this model+mic).
#     r24.6 (WO-02) also stops their probability mass vetoing the class that IS
#     picked — retired mass now leaves the share denominator
#     (ATHENA_EMOTION_RETIRED_IN_DENOM=1 restores the r24.5 arithmetic).
#   • Per-class floors are applied as max(floor, ATHENA_EMOTION_SHARE) against
#     the picked class's share of candidate mass — see the gating knobs below.
#       MIN=0.50        base floor for any class not listed
#       MIN_HAPPY=0.65  0.80 -> 0.65 (WO-03): at 0.80 the knob was DEAD — the old
#                       0.90 share bar discarded it via max(). 0.65 pairs with
#                       SHARE=0.65. S19 replay (WO-02+03): 10 EMIT {sad 9,
#                       happy 1}, all 9 original sad emissions survive.
#       MIN_SAD=0.95    sad is the negative "sink" on this mic; the high
#                       precision floor is kept from the S19-validated set.
#       MIN_ANGRY=0.65  0.80 -> 0.65 — same dead-knob history and fix (WO-03).
#   • For the record: "surprised never exceeded 0.008" stopped being true in
#     S19, which measured surprised=0.202 at 15:19:19 — the strongest positive-
#     side decode of that session. The class stays retired on precision grounds,
#     not because the model cannot reach it.
#
# CALIBRATION (ATHENA_EMOTION_CALIBRATION=1): per-class precision floors
# (happy/sad/disgusted = 0.95/0.90/0.90) for measuring individual classes from
# the debug distribution. Same collapse setting as production (see above).
#
#   production:   ./launch-athena-397b.sh
#   calibration:  ATHENA_EMOTION_CALIBRATION=1 ./launch-athena-397b.sh
# talk-llama echoes the effective floors at startup ("emotion: floors ..." — on
# stderr, so as of r24.6 (WO-53) it lands in athena-diag.log).
ATHENA_EMOTION_CALIBRATION="${ATHENA_EMOTION_CALIBRATION:-0}"
if [ "$ATHENA_EMOTION_CALIBRATION" = "1" ]; then
    # Per-class calibration: specific labels, precision floors.
    export ATHENA_EMOTION_COLLAPSE_NEGATIVE=0
    export ATHENA_EMOTION_MIN=0.50
    export ATHENA_EMOTION_MIN_HAPPY=0.95
    export ATHENA_EMOTION_MIN_SAD=0.90
    export ATHENA_EMOTION_MIN_DISGUSTED=0.90
else
    # Production floor set (per-class emissions; collapse OFF — see above).
    export ATHENA_EMOTION_COLLAPSE_NEGATIVE=0
    export ATHENA_EMOTION_MIN=0.50
    export ATHENA_EMOTION_MIN_HAPPY=0.65   # WO-03: was 0.80, unreachable under the 0.90 share bar
    export ATHENA_EMOTION_MIN_SAD=0.95
    export ATHENA_EMOTION_MIN_ANGRY=0.65   # WO-03: was 0.80, same dead knob
    export ATHENA_EMOTION_MIN_SURPRISED=1.01   # retired: true and false readings overlap
    export ATHENA_EMOTION_MIN_FEARFUL=1.01   # retired: true and false readings overlap
    export ATHENA_EMOTION_MIN_DISGUSTED=1.01   # retired: true and false readings overlap

fi

# The knobs that actually GATE emission (added r24.6, WO-66 — the launcher never
# set these and the old comment never mentioned them; a class emits only if ALL
# of them pass). Values = the r24.6 code defaults, exported so the whole gate is
# visible — and regenerable — in one place:
export ATHENA_EMOTION_SHARE=0.65      # share bar: the picked class's share of
                                      #   CANDIDATE mass must reach
                                      #   max(SHARE, its per-class MIN).
                                      #   WO-03: 0.90 -> 0.65 (at 0.90 it
                                      #   silently overrode both 0.80 MINs).
export ATHENA_EMOTION_EVIDENCE=0.10   # evidence gate: total candidate mass
                                      #   (non neutral/other/unk, non-retired)
                                      #   must reach this before anything emits.
export ATHENA_EMOTION_NOISE=0.02      # noise gate: the picked class's absolute
                                      #   probability must clear this floor.
export ATHENA_EMOTION_ABSOLUTE=0      # 1 = the pre-r21-full.8 rule (floors on
                                      #   the raw posterior, share rule
                                      #   bypassed) — kept as an A/B switch.

# Production GPU env — defaulted HERE, not only in the desktop icon (CHANGES.MD §16
# Fix 4). Previously only `Athena 397B.desktop` set these, so a direct shell launch
# silently reverted to pinned host buffers + no-MPS — a different (and less tested)
# memory configuration than every production run. Overridable per-run as usual.
#   GGML_CUDA_NO_PINNED=1  all GPU<->host staging on pageable memory (no cudaMallocHost)
#   ATHENA_MPS=1           single shared CUDA context for the three GPU clients
export GGML_CUDA_NO_PINNED="${GGML_CUDA_NO_PINNED:-1}"
ATHENA_MPS="${ATHENA_MPS:-1}"

# Temp files
SPEAK_FILE="$ATHENA_DIR/speakfile.temp"
TRIGGER_FILE="$ATHENA_DIR/speak_tts.txt"
DONE_FILE="$ATHENA_DIR/speak_tts.done"
STOP_FILE="$ATHENA_DIR/speak_tts.stop"   # barge-in: talk-llama -> orpheus-speak abort request
WAV_FILE="/dev/shm/orpheus_tts.wav"
OPT_CACHE="${SNAC_MODEL}.optimized"

# Cross-session memory + long-term personality. talk-llama reads its two injected
# text files (memory.txt, personality.txt) and two sidecars (memory.state.tsv,
# personality.ledger) plus a "meta" timestamp here. Consolidation writes them
# at EVERY session end that had a session — a spoken goodbye or a Ctrl+C alike
# (brain C48/C49, r19.6); the sentence that used to stand here, "only on a
# graceful goodbye (a Ctrl+C is SIGTERM'd before that point)", predates C49 and
# was the wrong model of shutdown that sized the grace below (r24.20 review,
# INTEGRATION F1). Comment this out (or set empty) to run with memory fully
# disabled — byte-for-byte the old behavior.
MEMORY_DIR="$ATHENA_DIR/athena_memory"

# PIDs for cleanup
PID_LLAMA=""
PID_ORPHEUS=""
PID_TALK=""
PID_TALK_LOG=""
PGID_TALK=""
TALK_REAPED=0
ATHENA_CLEANING=0
ATHENA_FORCE_STOP=0
ATHENA_TALK_STARTING=0
ATHENA_PENDING_SIGNAL=0
PID_WATCHDOG=""          # llama-server health-watchdog subshell (CHANGES.MD §12)
PID_KEEPALIVE=""         # llama-server idle-keepalive subshell

# ── llama-server watchdog / keepalive / coredumps (all A/B-overridable) ──────────
# The 20260701-123219 run proved llama-server can die a CONTAINED userspace libcuda
# GPF while MPS keeps the brain + orpheus-speak healthy — a restart restores TTS in
# seconds at ZERO steady-state cost (acts only on failure).
ATHENA_WATCHDOG="${ATHENA_WATCHDOG:-1}"                 # 0 disables auto-restart
ATHENA_KEEPALIVE="${ATHENA_KEEPALIVE:-1}"               # 0 disables the idle 1-token poke
# DEFAULT 0 since CHANGES.MD §19: on this corrupting box a client's core dump is a
# large ext4 write whose folio allocations draw poisoned pages — the 20260702-124334
# fatal oops was INSIDE the coredump path (do_coredump→elf_core_dump→...→folio_alloc),
# i.e. the dump turned a survivable client abort (Xid→watchdog restart) into a reboot.
# We already have the libcuda-GPF backtraces from prior runs. Set =1 to re-enable for
# forensics on a NON-corrupting kernel.
ATHENA_COREDUMP="${ATHENA_COREDUMP:-0}"                 # 1 re-enables llama/orpheus core dumps
LLAMA_PIDFILE="/dev/shm/athena-llama-server.$$.pid"     # live llama-server PID (survives restarts)
ORPHEUS_PIDFILE="/dev/shm/athena-orpheus-speak.$$.pid"  # live orpheus-speak PID (survives restarts, §20)
LLAMA_WEDGE="/dev/shm/athena-llama-wedge.$$"            # keepalive-detected decode wedge (§20: /health is
                                                        # hardcoded "ok" — it cannot see a stuck decode)
LLAMA_WD_STOP="/dev/shm/athena-wd-stop.$$"              # teardown sentinel (blocks respawn/poke, both watchdogs)
PID_ORPHWD=""                                           # orpheus-speak watchdog subshell (§20)
WD_INTERVAL="${ATHENA_WD_INTERVAL:-2}"                  # health-poll cadence (s)
WD_FAIL_THRESHOLD="${ATHENA_WD_FAIL_THRESHOLD:-2}"      # consecutive /health misses ⇒ wedged
WD_MAX_RESTARTS="${ATHENA_WD_MAX_RESTARTS:-5}"          # cap within …
WD_WINDOW="${ATHENA_WD_WINDOW:-300}"                    # … rolling window (s) before giving up
CORE_ULIMIT=0            # raised to 'unlimited' for llama/orpheus ONLY (never the 161 GiB brain)
CORE_PATTERN_SAVE=""     # original /proc/sys/kernel/core_pattern, to restore on exit
PID_GPUMON=""

# ── Diagnostics ───────────────────────────────────────────────────────────────
# DEFAULT 0 (clean-system deployment — no forensic instrumentation). When set to 1,
# ATHENA_DIAG captures per-run diagnostics into a timestamped dir AND spawns the
# GPU/Xid sampler (athena-gpu-monitor.sh, which READS THE KERNEL LOG via dmesg/kmsg)
# AND runs the prior-crash pstore harvest + nvidia-bug-report + pstore-clear below.
# Contents when on:
#   orpheus-server.log  llama-server's own stdout/stderr (else /dev/null)
#   gpu.csv             2 Hz GPU clocks/power/temp/util/VRAM sample
#   gpu-proc.log        1 Hz per-process VRAM (shows WHICH process's memory drops)
#   gpu-throttle.log    1 Hz active clock-event/throttle reasons
#   xid.log             kernel Xid/NVRM faults (GPU errors invisible to the apps)
# Set ATHENA_DIAG=1 to re-enable (e.g. one bring-up run to capture the server log).
ATHENA_DIAG="${ATHENA_DIAG:-0}"

# DEFAULT 0 (clean-system deployment). When set to 1, orpheus-speak dumps, per TTS
# session, the raw model tokens (.tokens), parsed audio codes (.codes), the live
# decoded audio (.wav) and a summary (.meta) into <run-dir>/tts-capture — the data
# to root-cause TTS garbling (model output vs SSE parse vs on-GPU SNAC decode).
# Re-decode a captured .codes offline with: orpheus-speak --snac <model> --snac-cpu
#   --decode-codes <file.codes>  (writes <file>.redecode.wav for an A/B vs the .wav).
# Set ATHENA_TTS_CAPTURE=1 to re-enable (each captured session writes a few MB of WAV).
ATHENA_TTS_CAPTURE="${ATHENA_TTS_CAPTURE:-0}"
GPU_MONITOR="$ATHENA_DIR/athena-gpu-monitor.sh"
DIAG_DIR="${ATHENA_DIAG_DIR:-$ATHENA_DIR/athena-diag}"
RUN_DIR="$DIAG_DIR/$(date +%Y%m%d-%H%M%S)"
ORPHEUS_SERVER_LOG="/dev/null"
if [ "$ATHENA_DIAG" != "0" ]; then
    mkdir -p "$RUN_DIR" 2>/dev/null && ORPHEUS_SERVER_LOG="$RUN_DIR/orpheus-server.log" \
        || { RUN_DIR=""; echo "[launch-athena] WARNING: could not create $DIAG_DIR — diagnostics off"; ATHENA_DIAG=0; }
else
    # Diagnostics off: no run dir is created, so leave RUN_DIR empty. Consumers
    # already handle empty (watchdogs fall back to /dev/shm/watchdog.log; MPS log
    # to /tmp; core-dump and sampler blocks skip) — a non-empty path to a dir that
    # was never mkdir'd caused "watchdog.log: No such file or directory" spam.
    RUN_DIR=""
fi

# ── Per-writer log streams (r24.6 WO-53, S19 G8) ─────────────────────────────
# One `ts` per WRITER, not one `ts` for the whole session. S19's athena.log had
# 2,028 records glued mid-line to another writer's unterminated output, because
# talk-llama's token stream (stdout, flushed with NO newline — that is how her
# speech renders incrementally), talk-llama's diag threads (stderr) and the
# orpheus-speak PROCESS all shared one pipe into one `ts`. No in-process fix can
# be complete (orpheus-speak is a separate process; 989 of the glued records
# were its), and newline-terminating the token stream destroys the incremental
# display — so each writer gets its own timestamper instead (the pattern
# start_llama_server already uses for orpheus-server.log). Measured in the
# WO-53 harness: 0 glued records, speech display unchanged.
#   athena.log            (unchanged) talk-llama stdout — the conversation, her
#                         speech streaming token-by-token — via the caller's
#                         existing `| ts | tee athena.log` pipe (the .desktop).
#   orpheus-speak.log     everything orpheus-speak prints (chunks, prosody,
#                         --diag fill heartbeat), stamped by its own _ts.
#   athena-diag.log       talk-llama stderr ([mind~], barge monitor:, emotion:,
#                         [silero]/endpoint, loader spew), own _ts.
# Both new logs APPEND across sessions (and across watchdog restarts — that is
# load-bearing); rotate or delete them freely between runs. For a unified view,
# merge by timestamp offline:  sort -m athena.log orpheus-speak.log athena-diag.log
ORPHEUS_SPEAK_LOG="$ATHENA_DIR/orpheus-speak.log"
ATHENA_DIAG_LOG="$ATHENA_DIR/athena-diag.log"

# Prior-crash harvest: if the last boot ended in a kernel panic, its RIP+trace was
# written to efi-pstore (armed by apply-crash-capture.sh) and survives into this boot.
# Pull it — plus a full nvidia-bug-report — into athena-diag/crash-<ts>/ for the
# #1064/#1111 report, then CLEAR pstore (the EFI var store is tiny; a full one blocks
# the next dump). Runs once per launch, before we touch the GPU. Never fatal.
# Two sources, because systemd-pstore.service RACES us: at boot it moves the EFI
# records out of /sys/fs/pstore into /var/lib/systemd/pstore/<epoch>/ before this
# preflight ever runs (which is why crash-<ts>/ dirs never appeared for the earlier
# panics — the dumps were all sitting in the systemd archive). Harvest BOTH: any raw
# records still in /sys/fs/pstore, and any systemd-archived dirs newer than our marker.
if [ "$ATHENA_DIAG" != "0" ]; then
    PSTORE_MARK="$DIAG_DIR/.pstore-harvest-marker"
    PSTORE_RAW=""; PSTORE_ARCH=""
    ls /sys/fs/pstore/* >/dev/null 2>&1 && PSTORE_RAW=1
    if [ -d /var/lib/systemd/pstore ]; then
        if [ -e "$PSTORE_MARK" ]; then
            [ -n "$(find /var/lib/systemd/pstore -mindepth 1 -maxdepth 1 -newer "$PSTORE_MARK" -print -quit 2>/dev/null)" ] && PSTORE_ARCH=1
        else
            [ -n "$(find /var/lib/systemd/pstore -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ] && PSTORE_ARCH=1
        fi
    fi
    if [ -n "$PSTORE_RAW$PSTORE_ARCH" ]; then
        CRASH_DIR="$DIAG_DIR/crash-$(date +%Y%m%d-%H%M%S)"
        if mkdir -p "$CRASH_DIR" 2>/dev/null; then
            echo "[launch-athena] prior crash dump(s) found — harvesting to $CRASH_DIR"
            [ -n "$PSTORE_RAW" ] && { sudo -n cp -a /sys/fs/pstore/. "$CRASH_DIR"/raw-pstore/ 2>/dev/null \
                || echo "  (could not copy /sys/fs/pstore — need sudo)"; }
            if [ -n "$PSTORE_ARCH" ]; then
                mkdir -p "$CRASH_DIR/systemd-pstore"
                sudo -n cp -a /var/lib/systemd/pstore/. "$CRASH_DIR"/systemd-pstore/ 2>/dev/null \
                    || echo "  (could not copy systemd pstore archive — need sudo)"
            fi
            if command -v nvidia-bug-report.sh >/dev/null 2>&1; then
                sudo -n nvidia-bug-report.sh --output-file "$CRASH_DIR/nvidia-bug-report" >/dev/null 2>&1 \
                    && echo "  nvidia-bug-report saved" || echo "  (nvidia-bug-report skipped)"
            fi
            sudo -n chown -R "$(id -u):$(id -g)" "$CRASH_DIR" 2>/dev/null || true
            [ -n "$PSTORE_RAW" ] && sudo -n rm -f /sys/fs/pstore/* 2>/dev/null && echo "  /sys/fs/pstore cleared for the next dump"
            touch "$PSTORE_MARK" 2>/dev/null
            echo "  panic backtraces: look for dmesg.txt under $CRASH_DIR/systemd-pstore/<epoch>/"
        fi
    fi
fi

# Line timestamper for captured child logs: moreutils ts if present (matches the
# [YYYY-MM-DD HH:MM:SS.ffffff] format of this script's own log), else passthrough.
# r24.20 review (INTEGRATION F1, measured): this writer is a process
# substitution, and bash (5.2) gives a process-substitution child the DEFAULT
# SIGINT/SIGHUP dispositions — only `&` jobs inherit the async ignore. A
# terminal Ctrl+C (or a closed window) therefore killed the brain's stderr
# writer, the brain's next fprintf(stderr) — the "consolidating memory" line
# itself — raised SIGPIPE, and the brain died 0.2 s into every Ctrl+C-ended
# session, in every launch form, before any consolidation. The writer now
# ignores the terminal's two signals and ends the way r24.17 designed it to:
# on EOF, when its writer closes the pipe (drained/terminated by
# _athena_drain_brain_group). TERM stays trappable so that drain still works.
# Unconditional (both launch arms): a log writer dying with the terminal is a
# data-loss defect of the WO-53 split, not a supervision policy.
_ts() {
    trap '' INT HUP
    if command -v ts >/dev/null 2>&1; then
        if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then exec ts '[%Y-%m-%d %H:%M:%.S]'; fi
        ts '[%Y-%m-%d %H:%M:%.S]'
    else
        if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then exec cat; fi
        cat
    fi
}

# Track whether GPU clocks were successfully locked (for cleanup).
# Cleanup owns only settings successfully changed by this launcher.
GPU_MEMORY_CLOCKS_CHANGED=false
GPU_GRAPHICS_CLOCKS_CHANGED=false
declare -A ATHENA_GPU_PERSISTENCE_ORIGINAL=()
declare -A ATHENA_GPU_POWER_ORIGINAL=()
declare -A ATHENA_GPU_PERSISTENCE_CHANGED=()
declare -A ATHENA_GPU_POWER_CHANGED=()

# Track whether THIS launcher started a CUDA MPS daemon, so cleanup only quits
# one we own. Values: false | true (we started it) | reused (pre-existing daemon).
MPS_STARTED=false

# Power/EPP state captured at launch so the stop path can revert it exactly.
# Save values before ANY governor/profile change: changing a governor can
# itself change EPP. Only successful mutations are restored, once.
declare -A ATHENA_SYSTEM_ORIGINAL=()
declare -A ATHENA_SYSTEM_CHANGED=()
declare -A ATHENA_SYSTEM_COUPLED=()
ATHENA_SYSTEM_ORDER=()
PPD_WAS_ACTIVE=false  # power-profiles-daemon was running and we stopped it
TUNED_SAVE=""         # original tuned profile name, if we switched it

# ── GPU performance setup ─────────────────────────────────────────────────────

# Query each device before changing it. A missing/unsupported value does not
# authorize guessing a default at exit; leave that setting alone and say so.
_athena_gpu_snapshot() {
    local rows index persistence power
    rows="$(nvidia-smi --query-gpu=index,persistence_mode,power.limit --format=csv,noheader,nounits 2>/dev/null)" || return 1
    while IFS=, read -r index persistence power; do
        index="${index//[[:space:]]/}"; persistence="${persistence//[[:space:]]/}"; power="${power//[[:space:]]/}"
        [[ "$index" =~ ^[0-9]+$ ]] || continue
        case "$persistence" in Enabled|enabled|1) persistence=1;; Disabled|disabled|0) persistence=0;; *) persistence="";; esac
        if [ -n "$persistence" ] && [ -z "${ATHENA_GPU_PERSISTENCE_ORIGINAL[$index]+saved}" ]; then
            ATHENA_GPU_PERSISTENCE_ORIGINAL["$index"]="$persistence"
        fi
        if [[ "$power" =~ ^[0-9]+([.][0-9]+)?$ ]] && [ -z "${ATHENA_GPU_POWER_ORIGINAL[$index]+saved}" ]; then
            ATHENA_GPU_POWER_ORIGINAL["$index"]="$power"
        fi
    done <<< "$rows"
}
_athena_gpu_persistence_on() {
    local index
    for index in "${!ATHENA_GPU_PERSISTENCE_ORIGINAL[@]}"; do
        [ "${ATHENA_GPU_PERSISTENCE_ORIGINAL[$index]}" != 1 ] || continue
        if sudo nvidia-smi -i "$index" -pm 1 2>/dev/null; then
            ATHENA_GPU_PERSISTENCE_CHANGED["$index"]=1
            echo "  GPU $index persistence mode: enabled for this session"
        fi
    done
    return 0
}
_athena_gpu_power_set() {
    local desired="$1" index changed=0
    for index in "${!ATHENA_GPU_POWER_ORIGINAL[@]}"; do
        if sudo nvidia-smi -i "$index" -pl "$desired" 2>/dev/null; then
            ATHENA_GPU_POWER_CHANGED["$index"]=1
            changed=1
        fi
    done
    [ "$changed" = 1 ]
}
_athena_gpu_restore_saved() {
    local index
    for index in "${!ATHENA_GPU_POWER_CHANGED[@]}"; do
        if sudo nvidia-smi -i "$index" -pl "${ATHENA_GPU_POWER_ORIGINAL[$index]}" 2>/dev/null; then
            unset 'ATHENA_GPU_POWER_CHANGED[$index]'
        fi
    done
    for index in "${!ATHENA_GPU_PERSISTENCE_CHANGED[@]}"; do
        if sudo nvidia-smi -i "$index" -pm "${ATHENA_GPU_PERSISTENCE_ORIGINAL[$index]}" 2>/dev/null; then
            unset 'ATHENA_GPU_PERSISTENCE_CHANGED[$index]'
        fi
    done
}

setup_gpu_performance() {
    echo "[launch-athena] configuring GPU for maximum performance..."

    if ! command -v nvidia-smi &>/dev/null; then
        echo "[launch-athena] WARNING: nvidia-smi not found, skipping GPU tuning"
        return
    fi

    if ! _athena_gpu_snapshot; then
        echo "  WARNING: initial GPU persistence/power state could not be read; leaving those controls unchanged"
    fi
    _athena_gpu_persistence_on

    # Dynamic Boost / nvidia-powerd: NOT auto-started by default (ATHENA_START_POWERD=0).
    # On the original Blackwell box the GPU<->SBIOS handshake wants nvidia-powerd driving
    # the ~95 W budget (unmanaged Dynamic Boost was a suspected Xid 69 trigger), so the
    # launcher used to start it if inactive. That's now opt-in — the launcher no longer
    # starts this system service on its own. Set ATHENA_START_POWERD=1 to restore the old
    # start-if-inactive behavior. Either way we only REPORT its state; we never stop it.
    ATHENA_START_POWERD="${ATHENA_START_POWERD:-0}"
    if command -v systemctl &>/dev/null && systemctl list-unit-files nvidia-powerd.service &>/dev/null; then
        if systemctl is-active --quiet nvidia-powerd; then
            echo "  nvidia-powerd: active (Dynamic Boost managed)"
        elif [ "$ATHENA_START_POWERD" = "1" ]; then
            echo "  nvidia-powerd: NOT active — attempting to start (ATHENA_START_POWERD=1)"
            sudo systemctl start nvidia-powerd 2>/dev/null
            if systemctl is-active --quiet nvidia-powerd; then
                echo "  nvidia-powerd: started"
            else
                echo "  WARNING: nvidia-powerd could not be started — GPU power management is degraded"
            fi
        else
            echo "  nvidia-powerd: NOT active — leaving as-is (ATHENA_START_POWERD=0 default; set =1 to auto-start)"
        fi
    fi

    # Graphics clock: deliberately NOT locked. Sweep 2026-06 (orpheus-powertune.sh
    # T2): unlocked = 167.2 tok/s vs ~158-165 for every fixed lock (900-3090).
    # Under the 95 W platform cap the boost governor beats any fixed V/F point.
    #
    # Memory clock: DEFAULT IS NOW 0 (driver-managed, no lock) — reverted CHANGES.MD §12.
    # The §10 D2 floor mode [13000,max] was a NO-OP in this passthrough container: the
    # 20260701-123219 gpu.csv shows memclk still dropped to 9001 in 45% of samples
    # despite the lock (the dead SBIOS handshake makes -lmc's floor unenforceable, same
    # root cause as -pl being rejected). Under load the driver runs 13801 either way, so
    # the floor recovered nothing and only logged a "floored at 13000" success the
    # telemetry falsifies. Default 0 = tell the truth; the levers stay for A/B:
    #   ATHENA_LOCK_MEMCLK=0        driver-managed, no lock (DEFAULT)
    #   ATHENA_LOCK_MEMCLK=1        rigid pin to max 14001 (Xid-69 suspect; A/B only)
    #   ATHENA_LOCK_MEMCLK=<floor>  lock range [<floor>, max] (floor-mode A/B)
    ATHENA_LOCK_MEMCLK="${ATHENA_LOCK_MEMCLK:-0}"
    local max_mem
    max_mem=$(nvidia-smi --query-gpu=clocks.max.memory --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')

    if [[ "$ATHENA_LOCK_MEMCLK" == "1" && -n "$max_mem" && "$max_mem" != "[N/A]" ]]; then
        if sudo nvidia-smi -lmc "$max_mem" 2>/dev/null; then
            GPU_MEMORY_CLOCKS_CHANGED=true
            echo "  memory clocks PINNED to ${max_mem} MHz (ATHENA_LOCK_MEMCLK=1 — Xid 69 suspect)"
        fi
    elif [[ "$ATHENA_LOCK_MEMCLK" != "0" && "$ATHENA_LOCK_MEMCLK" != "1" ]]; then
        # Floor mode: lock a range [floor, max] so the clock can still float down for
        # power but never drops to the unstable low state during power transitions.
        if [[ -n "$max_mem" && "$max_mem" != "[N/A]" ]] && \
           sudo nvidia-smi -lmc "${ATHENA_LOCK_MEMCLK},${max_mem}" 2>/dev/null; then
            GPU_MEMORY_CLOCKS_CHANGED=true
            echo "  memory clocks floored at ${ATHENA_LOCK_MEMCLK} MHz (range ${ATHENA_LOCK_MEMCLK}-${max_mem})"
        fi
    else
        echo "  memory clocks: left to the driver (not pinned) — default; set ATHENA_LOCK_MEMCLK=1 to A/B test"
    fi

    # ── Power ceiling: DO NOT touch -pl by default (reverted, CHANGES.MD §12) ──
    # History: §10 D2 raised -pl toward max; §11 F3 lowered it ~10 W below default.
    # The 20260701-123219 run PROVED both are pointless HERE: the SBIOS power
    # handshake is dead in this passthrough container ('PlatformRequestHandler failed
    # to get target temp/platform power mode from SBIOS' at boot), so nvidia-powerd
    # owns the ceiling outright and 'nvidia-smi -pl' is rejected ('not supported in
    # current scope'; readback stayed 105.35 W vs an 85 W request). And it wasn't
    # needed: that run rode the SAME ~95 W wall as the 074343 crash yet threw NO Xid —
    # stability came from S0ix-off (D0 pinning), not from any clock/power tuning. So by
    # default we issue NO -pl at all (a no-op here, a perf tax anywhere it IS honored).
    #   ATHENA_GPU_POWER_LIMIT=<W>  explicit cap, opt-in A/B only (perf risk if honored)
    #   ATHENA_GPU_POWER_MAX=1      opt-in: raise to board max
    local max_pl
    max_pl=$(nvidia-smi --query-gpu=power.max_limit --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ' | cut -d. -f1)
    [[ "${ATHENA_GPU_POWER_MAX:-0}" == "1" ]] && ATHENA_GPU_POWER_LIMIT="${ATHENA_GPU_POWER_LIMIT:-$max_pl}"
    if [[ -n "${ATHENA_GPU_POWER_LIMIT:-}" && "$ATHENA_GPU_POWER_LIMIT" != "[N/A]" ]]; then
        if _athena_gpu_power_set "$ATHENA_GPU_POWER_LIMIT"; then
            local now_pl
            now_pl=$(nvidia-smi --query-gpu=power.limit --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
            echo "  power limit set to ${ATHENA_GPU_POWER_LIMIT} W (now reads: ${now_pl} W — if unchanged, nvidia-powerd holds its own ceiling and ignored -pl; try ATHENA_GPU_CLOCK_CAP)"
        else
            echo "  WARNING: power limit ${ATHENA_GPU_POWER_LIMIT} W rejected by the GPU"
        fi
    fi
    # Escalation lever only (unset by default): cap the graphics clock if -pl is
    # ignored and you need to force the GPU out of the power-cap regime for an A/B.
    #   ATHENA_GPU_CLOCK_CAP=1100   cap max graphics clock at 1100 MHz
    if [[ -n "${ATHENA_GPU_CLOCK_CAP:-}" ]]; then
        if sudo nvidia-smi -lgc "0,${ATHENA_GPU_CLOCK_CAP}" 2>/dev/null; then
            GPU_GRAPHICS_CLOCKS_CHANGED=true
            echo "  graphics clock capped at ${ATHENA_GPU_CLOCK_CAP} MHz (lower this until 'SW Power Cap' stops appearing in gpu-throttle.log)"
        else
            echo "  WARNING: graphics clock cap ${ATHENA_GPU_CLOCK_CAP} MHz rejected"
        fi
    fi

    echo "[launch-athena] GPU configured."
}

restore_gpu_performance() {
    echo "  restoring GPU to normal..."

    if ! command -v nvidia-smi &>/dev/null; then return; fi

    if $GPU_MEMORY_CLOCKS_CHANGED; then
        if sudo nvidia-smi -rmc 2>/dev/null; then
            GPU_MEMORY_CLOCKS_CHANGED=false
            echo "    memory clocks: session lock released"
        fi
    fi
    if $GPU_GRAPHICS_CLOCKS_CHANGED; then
        if sudo nvidia-smi -rgc 2>/dev/null; then
            GPU_GRAPHICS_CLOCKS_CHANGED=false
            echo "    graphics clocks: session lock released"
        fi
    fi
    _athena_gpu_restore_saved

}

# ── CUDA MPS (diagnostic: merge the per-process GPU contexts) ──────────────────
# ATHENA_MPS=1 routes llama-server, talk-llama and orpheus-speak through a single
# CUDA MPS server context instead of letting them time-slice the GPU as three
# separate contexts — the no-rebuild A/B for the Xid 69 multi-context hypothesis.
# Unset (default) exports nothing and starts no daemon: byte-identical behavior.
#
# Compute mode is deliberately left at DEFAULT. Do NOT set EXCLUSIVE_PROCESS: if
# the display is driven by the dGPU it would lose its context and the desktop
# would freeze. In DEFAULT mode MPS still serves the Athena clients while the
# compositor keeps its own context.

ATHENA_MPS_FORCE_DIRECTORY=""
declare -A ATHENA_MPS_FORCE_IDENTITIES=()

# A name match is only a candidate. Other MPS installations can run under this
# same account with different pipe directories. Verify the actual executable,
# real/effective UID, canonical endpoint directory and immutable start tick;
# repeat that verification before each signal so a recycled PID is not killed.
# Return 2 for a visible same-user MPS process whose endpoint cannot be read:
# it must not be signalled, nor may we assume the old pipe directory is unused.
_athena_mps_process_identity() {
    local pid="$1" directory="$2" line="" original="" executable="" key real effective rest
    local entry pipe="" cwd="" owner_seen=0
    local -a fields=()
    ATHENA_MPS_PROCESS_IDENTITY=""
    [[ "$pid" =~ ^[1-9][0-9]{0,9}$ ]] || return 1
    IFS= read -r line 2>/dev/null < /proc/self/stat || return 2
    [ "${line%% *}" = "$BASHPID" ] || return 2
    IFS= read -r line 2>/dev/null < "/proc/$pid/stat" || return 1
    read -r -a fields <<< "${line##*) }"
    [[ "${fields[19]:-}" =~ ^[0-9]+$ ]] && [ "${fields[0]:-Z}" != Z ] || return 1
    original="${fields[19]}"
    executable="$(readlink "/proc/$pid/exe" 2>/dev/null)" || return 1
    executable="${executable% (deleted)}"
    case "${executable##*/}" in nvidia-cuda-mps-control|nvidia-cuda-mps-server) ;; *) return 1 ;; esac
    while read -r key real effective rest; do
        if [ "$key" = Uid: ]; then
            [ "$real" = "$EUID" ] && [ "$effective" = "$EUID" ] || return 1
            owner_seen=1; break
        fi
    done 2>/dev/null < "/proc/$pid/status"
    [ "$owner_seen" = 1 ] || return 2
    [ -r "/proc/$pid/environ" ] || return 2
    while IFS= read -r -d '' entry; do
        case "$entry" in CUDA_MPS_PIPE_DIRECTORY=*) pipe="${entry#*=}"; break ;; esac
    done 2>/dev/null < "/proc/$pid/environ"
    [ -n "$pipe" ] || return 2
    if [[ "$pipe" != /* ]]; then
        cwd="$(readlink "/proc/$pid/cwd" 2>/dev/null)" || return 2
        pipe="$cwd/$pipe"
    fi
    pipe="$(readlink -m -- "$pipe" 2>/dev/null)" || return 2
    [ "$pipe" = "$directory" ] || return 1
    IFS= read -r line 2>/dev/null < "/proc/$pid/stat" || return 1
    read -r -a fields <<< "${line##*) }"
    [ "${fields[19]:-}" = "$original" ] && [ "${fields[0]:-Z}" != Z ] || return 1
    ATHENA_MPS_PROCESS_IDENTITY="$original:$executable"
}

_athena_mps_force_scoped() {
    local directory pid signal status unknown=0 remaining=0
    local -A identities=()
    ATHENA_MPS_SCOPED_COUNT=0
    [ -n "${CUDA_MPS_PIPE_DIRECTORY:-}" ] || return 1
    directory="$(readlink -m -- "$CUDA_MPS_PIPE_DIRECTORY" 2>/dev/null)" || return 1
    if [ "$ATHENA_MPS_FORCE_DIRECTORY" != "$directory" ]; then
        ATHENA_MPS_FORCE_DIRECTORY=""
        ATHENA_MPS_FORCE_IDENTITIES=()
    fi
    # -f is needed because Linux comm truncates these executable names. It
    # enumerates candidates only; no signal is ever sent through pgrep/pkill.
    for pid in $(pgrep -f 'nvidia-cuda-mps-(control|server)' 2>/dev/null); do
        if _athena_mps_process_identity "$pid" "$directory"; then
            # A retry may finish stopping the original processes. It cannot
            # acquire a replacement daemon that reused this PID or endpoint.
            if [ -n "$ATHENA_MPS_FORCE_DIRECTORY" ] &&
               [ "${ATHENA_MPS_FORCE_IDENTITIES[$pid]:-}" != "$ATHENA_MPS_PROCESS_IDENTITY" ]; then
                unknown=1; continue
            fi
            identities["$pid"]="$ATHENA_MPS_PROCESS_IDENTITY"
        else status=$?; [ "$status" != 2 ] || unknown=1; fi
    done
    if [ -z "$ATHENA_MPS_FORCE_DIRECTORY" ]; then
        ATHENA_MPS_FORCE_DIRECTORY="$directory"
        for pid in "${!identities[@]}"; do
            ATHENA_MPS_FORCE_IDENTITIES["$pid"]="${identities[$pid]}"
        done
    fi
    ATHENA_MPS_SCOPED_COUNT=${#identities[@]}
    if [ "$ATHENA_MPS_SCOPED_COUNT" -gt 0 ]; then
        echo "    forcing $ATHENA_MPS_SCOPED_COUNT MPS process(es) at $directory"
        for signal in TERM KILL; do
            for pid in "${!identities[@]}"; do
                if _athena_mps_process_identity "$pid" "$directory" &&
                   [ "$ATHENA_MPS_PROCESS_IDENTITY" = "${identities[$pid]}" ]; then
                    kill "-$signal" -- "$pid" 2>/dev/null || true
                fi
            done
            [ "$signal" != TERM ] || sleep 1
        done
        for pid in "${!identities[@]}"; do
            if _athena_mps_process_identity "$pid" "$directory"; then remaining=1
            else status=$?; [ "$status" != 2 ] || unknown=1; fi
        done
    fi
    [ "$remaining" = 0 ] && [ "$unknown" = 0 ]
}

# Process ownership does not transfer ownership of the caller's directory.
# NVIDIA documents control IPC as a named pipe/UNIX socket and the daemon PID
# in nvidia-cuda-mps-control.pid. After scoped process shutdown, unlink only
# those verified, same-user artifacts. Keep the directory, its mode, logs and
# all unknown files/endpoints; recursively removing it could erase caller data.
_athena_mps_clear_control() {
    local directory endpoint pidfile pid
    local -a artifacts=()
    [ -n "${CUDA_MPS_PIPE_DIRECTORY:-}" ] || return 1
    directory="$(readlink -e -- "$CUDA_MPS_PIPE_DIRECTORY" 2>/dev/null)" || return 1
    [ -d "$directory" ] && [ -O "$directory" ] || return 1
    endpoint="$directory/control"
    pidfile="$directory/nvidia-cuda-mps-control.pid"
    if [ -e "$endpoint" ] || [ -L "$endpoint" ]; then
        [ ! -L "$endpoint" ] && [ -O "$endpoint" ] &&
            { [ -p "$endpoint" ] || [ -S "$endpoint" ]; } || return 1
        artifacts+=("$endpoint")
    fi
    if [ -e "$pidfile" ] || [ -L "$pidfile" ]; then
        [ ! -L "$pidfile" ] && [ -f "$pidfile" ] && [ -O "$pidfile" ] || return 1
        # Bounded read: the whole file must be one PID plus whitespace, not a
        # same-named user document or a special file that could block cleanup.
        [ "$(stat -c %s -- "$pidfile" 2>/dev/null)" -le 64 ] 2>/dev/null || return 1
        pid="$(< "$pidfile")"
        [[ "$pid" =~ ^[[:space:]]*[1-9][0-9]{0,9}[[:space:]]*$ ]] || return 1
        artifacts+=("$pidfile")
    fi
    if [ "${#artifacts[@]}" -gt 0 ]; then rm -f -- "${artifacts[@]}"; fi
}

start_mps() {
    [ "${ATHENA_MPS:-0}" = "1" ] || return 0

    if ! command -v nvidia-cuda-mps-control >/dev/null 2>&1; then
        echo "[launch-athena] ATHENA_MPS=1 but nvidia-cuda-mps-control not found — continuing WITHOUT MPS"
        return 0
    fi

    # The MPS *server* ships in /usr/sbin (nvidia-cuda-mps-server), but Debian omits
    # /usr/sbin from the user PATH — so the control daemon (in /usr/bin) can't exec
    # the server: every server dies "exited with status 1" (empty server.log) and
    # clients fail with "MPS client failed to connect to the MPS control daemon or
    # the MPS server". Ubuntu keeps /usr/sbin on PATH, which is why MPS worked there.
    # Put it on PATH so the daemon we spawn below inherits it and can find the server.
    case ":$PATH:" in *":/usr/sbin:"*) : ;; *) export PATH="/usr/sbin:$PATH" ;; esac

    # Honor pre-set pipe/log dirs; otherwise default the pipe to the conventional
    # /tmp path and put the server log under the per-run diag dir when one exists
    # (so it lands next to the other run artifacts). These exports persist in the
    # launcher's shell, so every GPU process started below inherits them.
    export CUDA_MPS_PIPE_DIRECTORY="${CUDA_MPS_PIPE_DIRECTORY:-/tmp/nvidia-mps}"
    if [ -z "${CUDA_MPS_LOG_DIRECTORY:-}" ]; then
        if [ -n "$RUN_DIR" ] && [ -d "$RUN_DIR" ]; then
            export CUDA_MPS_LOG_DIRECTORY="$RUN_DIR/mps-log"
        else
            export CUDA_MPS_LOG_DIRECTORY="/tmp/nvidia-mps-log"
        fi
    fi
    mkdir -p "$CUDA_MPS_PIPE_DIRECTORY" 2>/dev/null || true

    # A pre-existing control endpoint does NOT prove a daemon is alive — it may be
    # a stale pipe left by a daemon that died without cleanup (a SIGKILLed
    # talk-llama, or a prior launch that "reused" it and never stopped it). The
    # bare `-e` test used to false-positive on such a pipe, silently skipping MPS
    # and letting the three GPU clients time-slice one context. Probe it instead:
    # a live daemon answers a status query; a stale/dead pipe errors or hangs
    # (hence `timeout`, which also covers a FIFO open-for-write block).
    if [ -e "$CUDA_MPS_PIPE_DIRECTORY/control" ]; then
        if echo "get_default_active_thread_percentage" \
             | timeout 5 nvidia-cuda-mps-control >/dev/null 2>&1; then
            MPS_STARTED=reused
            echo "[launch-athena] MPS: reusing LIVE daemon at $CUDA_MPS_PIPE_DIRECTORY (will NOT stop it on exit)"
            return 0
        fi
        echo "[launch-athena] MPS: stale control pipe at $CUDA_MPS_PIPE_DIRECTORY (no daemon responding) — clearing and starting fresh"
        if ! _athena_mps_force_scoped; then
            echo "[launch-athena] WARNING: MPS endpoint ownership/exit is unresolved — leaving its directory intact and continuing WITHOUT MPS"
            unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY
            MPS_STARTED=false
            return 0
        fi
        if ! _athena_mps_clear_control; then
            echo "[launch-athena] WARNING: stale MPS control artifacts are not safely owned — retaining the directory and continuing WITHOUT MPS"
            unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY
            MPS_STARTED=false
            return 0
        fi
    fi

    # Create the log dir only now, when WE are about to own a daemon. Doing it
    # earlier (or on the reuse path) leaves an empty mps-log/ that misleadingly
    # reads as "MPS ran" — a reused daemon logs to its own original dir, not this.
    mkdir -p "$CUDA_MPS_LOG_DIRECTORY" 2>/dev/null || true
    if nvidia-cuda-mps-control -d 2>/dev/null; then
        MPS_STARTED=true
        ATHENA_MPS_FORCE_DIRECTORY=""
        ATHENA_MPS_FORCE_IDENTITIES=()
        # The control daemon starting is NOT proof MPS works — the per-user server
        # still has to spawn and init CUDA. Force one and confirm it survives (a
        # broken server exits status 1 and get_server_list stays empty). On failure
        # tear MPS down and run WITHOUT it, so a bad MPS degrades to the known-good
        # direct-GPU path instead of bricking every CUDA client (orpheus-speak died
        # here before this guard existed).
        echo "start_server -uid $(id -u)" | timeout 5 nvidia-cuda-mps-control >/dev/null 2>&1 || true
        if echo "get_server_list" | timeout 5 nvidia-cuda-mps-control 2>/dev/null | grep -q '[0-9]'; then
            echo "[launch-athena] MPS: daemon + server up — pipe=$CUDA_MPS_PIPE_DIRECTORY log=$CUDA_MPS_LOG_DIRECTORY"
            echo "[launch-athena] MPS: llama-server / orpheus-speak / talk-llama will share one GPU context"
        else
            echo "[launch-athena] WARNING: MPS server won't start (server exited; unsupported here?) — continuing WITHOUT MPS"
            echo quit | timeout 5 nvidia-cuda-mps-control >/dev/null 2>&1 || true
            unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY
            MPS_STARTED=false
        fi
    else
        echo "[launch-athena] WARNING: failed to start MPS daemon — continuing WITHOUT MPS"
        unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY
        MPS_STARTED=false
    fi
}

# Graceful MPS shutdown. Only quits a daemon THIS launcher started (MPS_STARTED
# == true); a reused daemon is left running. Called from cleanup() after the GPU
# client processes are already killed, so 'quit' has nothing to drain and returns
# fast. Runs under cleanup's `set +e`, so a wedged quit can't abort teardown.
stop_mps() {
    [ "${MPS_STARTED:-false}" = "true" ] || return 0
    command -v nvidia-cuda-mps-control >/dev/null 2>&1 || return 0

    echo "  stopping MPS daemon..."
    # 'quit -t N' tells the daemon to shut its servers down and, crucially, to
    # FORCE any server still up after N seconds. Bare 'quit' instead WAITS on
    # clients and hangs with exponential backoff — the exact 230402 wedge, where a
    # SIGKILLed client left "Server 3039 has 1 active worker thread ... will not
    # shutdown" and the plain `quit` timed out (line-382 exit-124). timeout may be
    # absent (fallback). All returns are swallowed (`|| true`): teardown is best-
    # effort and the ERR trap is already cleared in cleanup().
    if command -v timeout >/dev/null 2>&1; then
        echo "quit -t 5" | timeout 8 nvidia-cuda-mps-control >/dev/null 2>&1 || true
    else
        echo "quit -t 5" | nvidia-cuda-mps-control >/dev/null 2>&1 || true
    fi
    # Keep bounded TERM/KILL recovery for a wedged worker, scoped to this
    # endpoint. A healthy daemon at another directory is never a wedge here.
    if _athena_mps_force_scoped; then
        if [ "$ATHENA_MPS_SCOPED_COUNT" -gt 0 ]; then
            _athena_mps_clear_control ||
                echo "    WARNING: MPS stopped, but unverified control artifacts were retained"
        fi
        MPS_STARTED=false
        echo "    MPS stopped"
    else
        echo "    WARNING: MPS process exit/ownership is unresolved — retaining its pipe directory"
    fi
}

# Gracefully detach one MPS client's CUDA context via the documented
# terminate_client control command BEFORE we SIGKILL the process, so the server's
# per-client worker thread DRAINS instead of wedging. In 230402, SIGKILLing
# orpheus-speak (3481) while it still held a live MPS-client context left the
# server with a stuck worker thread ("... will not shutdown"). Only acts on a
# daemon THIS launcher owns; all control I/O is timeout-bounded so a wedged server
# can't hang teardown. (rs:mps-fault-isolation §4; quit -t in stop_mps is the
# guaranteed backstop if the context is already reset/poisoned and won't drain.)
mps_terminate_client() {            # $1 = client pid
    [ "${MPS_STARTED:-false}" = "true" ] || return 0
    command -v nvidia-cuda-mps-control >/dev/null 2>&1 || return 0
    local cpid="$1" srv
    [ -n "$cpid" ] || return 0
    for srv in $(echo get_server_list | timeout 5 nvidia-cuda-mps-control 2>/dev/null); do
        if echo "get_client_list $srv" | timeout 5 nvidia-cuda-mps-control 2>/dev/null \
             | tr -s ' ' '\n' | grep -qx "$cpid"; then
            echo "terminate_client $srv $cpid" | timeout 5 nvidia-cuda-mps-control >/dev/null 2>&1 || true
        fi
    done
}

# ── CPU / system performance setup ────────────────────────────────────────────

# sysfs choice files expose "always [madvise] never"; the bracketed item
# is the writable current value. Ordinary scalar attributes need no parsing.
_athena_system_snapshot() {
    local path="$1" original
    [ -f "$path" ] || return 1
    [ -z "${ATHENA_SYSTEM_ORIGINAL[$path]+saved}" ] || return 0
    IFS= read -r original < "$path" || return 1
    if [[ "$original" =~ \[([^][]+)\] ]]; then original="${BASH_REMATCH[1]}"; fi
    [ -n "$original" ] || return 1
    ATHENA_SYSTEM_ORIGINAL["$path"]="$original"
    ATHENA_SYSTEM_ORDER+=("$path")
}
_athena_system_set() {
    local path="$1" desired="$2" current coupled
    _athena_system_snapshot "$path" || return 1
    if printf '%s\n' "$desired" | sudo tee "$path" >/dev/null 2>&1; then
        # A successful write may also restore the original during a repeated
        # setup; in that case no cleanup mutation remains necessary.
        if [ "$desired" = "${ATHENA_SYSTEM_ORIGINAL[$path]}" ] &&
           [ -z "${ATHENA_SYSTEM_COUPLED[$path]+coupled}" ]; then
            unset 'ATHENA_SYSTEM_CHANGED[$path]'
        else ATHENA_SYSTEM_CHANGED["$path"]=1; fi
        if [[ "$path" == */scaling_governor ]] && [ "$desired" != "${ATHENA_SYSTEM_ORIGINAL[$path]}" ]; then
            coupled="${path%/*}/energy_performance_preference"
            if [ -n "${ATHENA_SYSTEM_ORIGINAL[$coupled]+saved}" ]; then
                ATHENA_SYSTEM_COUPLED["$coupled"]=1
                ATHENA_SYSTEM_CHANGED["$coupled"]=1
            fi
        fi
        return 0
    fi
    # A failed EPP write may follow a successful governor/profile mutation
    # that already changed EPP. Retain that observed difference for rollback.
    if IFS= read -r current < "$path"; then
        if [[ "$current" =~ \[([^][]+)\] ]]; then current="${BASH_REMATCH[1]}"; fi
        if [ "$current" != "${ATHENA_SYSTEM_ORIGINAL[$path]}" ]; then
            ATHENA_SYSTEM_CHANGED["$path"]=1
        fi
    fi
    return 1
}
_athena_system_restore() {
    local path governor
    for path in "${ATHENA_SYSTEM_ORDER[@]}"; do
        [ -n "${ATHENA_SYSTEM_CHANGED[$path]+changed}" ] || continue
        if printf '%s\n' "${ATHENA_SYSTEM_ORIGINAL[$path]}" | sudo tee "$path" >/dev/null 2>&1; then
            governor=""
            if [[ "$path" == */energy_performance_preference ]]; then
                governor="${path%/*}/scaling_governor"
            fi
            # A pending manager or governor retry can reset EPP again. Keep
            # the saved value until all earlier coupled restores succeeded.
            if ! $PPD_WAS_ACTIVE && [ -z "${TUNED_SAVE:-}" ] &&
               { [ -z "$governor" ] || [ -z "${ATHENA_SYSTEM_CHANGED[$governor]+changed}" ]; }; then
                unset 'ATHENA_SYSTEM_CHANGED[$path]'
                unset 'ATHENA_SYSTEM_COUPLED[$path]'
            fi
        fi
    done
}
_athena_system_observe_changes() {
    local path current
    for path in "${ATHENA_SYSTEM_ORDER[@]}"; do
        IFS= read -r current < "$path" || continue
        if [[ "$current" =~ \[([^][]+)\] ]]; then current="${BASH_REMATCH[1]}"; fi
        if [ "$current" != "${ATHENA_SYSTEM_ORIGINAL[$path]}" ]; then
            ATHENA_SYSTEM_CHANGED["$path"]=1
        fi
    done
}

setup_system_performance() {
    echo "[launch-athena] configuring system for maximum performance..."

    # Snapshot the complete set before a profile/governor can alter a sibling
    # setting. The ordered restore puts governors before their coupled EPP.
    local saved_path
    for saved_path in /sys/firmware/acpi/platform_profile \
        /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor \
        /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference \
        /sys/devices/system/cpu/cpu*/cpuidle/state*/disable \
        /sys/kernel/mm/transparent_hugepage/enabled \
        /sys/kernel/mm/transparent_hugepage/defrag /proc/sys/kernel/numa_balancing; do
        _athena_system_snapshot "$saved_path" || true
    done

    # ── Power-profile manager: the silent HWP trap ────────────────────────────
    # power-profiles-daemon (default on GNOME/KDE/Ubuntu desktops) can pin a
    # DEGRADED hardware P-state at the MSR level even while scaling_governor and
    # EPP both read "performance" — a documented 20-30% TG loss that varies
    # between boots and is invisible to every sysfs check. Take whichever manager
    # is active out of the loop for the session (so the governor/EPP writes below
    # actually hold); the stop path restores it. No-op if neither is running.
    if command -v systemctl &>/dev/null && systemctl is-active --quiet power-profiles-daemon 2>/dev/null; then
        if sudo systemctl stop power-profiles-daemon 2>/dev/null; then
            PPD_WAS_ACTIVE=true
            echo "  power-profiles-daemon: stopped for session (was free to override HWP)"
        else
            echo "  power-profiles-daemon: active but stop failed (continuing)"
        fi
    elif command -v tuned-adm &>/dev/null && systemctl is-active --quiet tuned 2>/dev/null; then
        TUNED_SAVE="$(tuned-adm active 2>/dev/null | sed -n 's/^Current active profile: //p')"
        if [ -n "$TUNED_SAVE" ] && [ "$TUNED_SAVE" != "throughput-performance" ]; then
            if sudo tuned-adm profile throughput-performance 2>/dev/null; then
                echo "  tuned: throughput-performance (was $TUNED_SAVE)"
            else
                echo "  tuned: could not switch profile (was $TUNED_SAVE)"
                TUNED_SAVE=""
            fi
        else
            TUNED_SAVE=""   # already optimal or unreadable — nothing to revert
        fi
    fi

    # ── ACPI platform profile → performance ──────────────────────────────────
    # The EC-level budget knob (Lenovo: low-power/balanced/performance). It caps
    # SUSTAINED package power and the dGPU TGP share — governor/EPP writes below
    # cannot override the EC. With PPD stopped above nothing manages it, and
    # stuck at low-power the EC clamps the GPU under combined Qwen+Orpheus load
    # (measured 2026-07-04 15:24 run: Orpheus 47-66 tok/s vs the 82 tok/s
    # realtime floor → 830 stalls / 33 s injected silence). Must run AFTER the
    # PPD stop so PPD can't immediately re-assert its own profile. Restored on
    # exit. ATHENA_PLATFORM_PROFILE=<name> picks another profile; ="" skips.
    ATHENA_PLATFORM_PROFILE="${ATHENA_PLATFORM_PROFILE-performance}"
    local pp=/sys/firmware/acpi/platform_profile
    if [ -z "$ATHENA_PLATFORM_PROFILE" ]; then
        :   # explicitly skipped
    elif [ ! -f "$pp" ]; then
        echo "  platform profile: interface not present (skipped)"
    else
        local pp_cur pp_choices
        pp_cur="$(cat "$pp" 2>/dev/null)"
        pp_choices="$(cat "${pp}_choices" 2>/dev/null)"
        if [ "$pp_cur" = "$ATHENA_PLATFORM_PROFILE" ]; then
            _athena_system_set "$pp" "$ATHENA_PLATFORM_PROFILE" || true
            echo "  platform profile: already $pp_cur"
        elif ! printf '%s' " $pp_choices " | grep -qF " $ATHENA_PLATFORM_PROFILE "; then
            echo "  platform profile: '$ATHENA_PLATFORM_PROFILE' not in choices ($pp_choices) — skipped"
        elif _athena_system_set "$pp" "$ATHENA_PLATFORM_PROFILE"; then
            echo "  platform profile: $ATHENA_PLATFORM_PROFILE (was $pp_cur)"
        else
            echo "  WARNING: platform profile write failed (still $pp_cur — GPU may be EC-clamped under sustained load)"
        fi
    fi

    # CPU governor → performance
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        _athena_system_set "$gov" performance || true
    done
    echo "  CPU governor: performance"

    # EPP (energy_performance_preference) → performance. On intel_pstate the
    # governor alone does NOT pin EPP; a non-performance EPP silently caps turbo.
    # This is the half of the fix the launcher was missing. Save each core's
    # original so the stop path reverts exactly (intel_pstate default is usually
    # balance_performance). Files are world-readable, so the read needs no sudo;
    # the write does.
    local epp epp_any=false
    for epp in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
        [ -f "$epp" ] || continue
        if _athena_system_set "$epp" performance; then epp_any=true; fi
    done
    if $epp_any; then
        echo "  EPP: performance (now: $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null))"
    else
        echo "  EPP: not exposed by this CPU/driver (skipped)"
    fi

    # Disable deep C-states (best effort — some sysfs files are read-only
    # depending on CPU/kernel; e.g. Arrow Lake-HX rejects writes to state0)
    for state in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do
        _athena_system_set "$state" 1 || true
    done
    echo "  C-states: disabled (best effort)"

    # Enable transparent huge pages
    _athena_system_set /sys/kernel/mm/transparent_hugepage/enabled always || true
    _athena_system_set /sys/kernel/mm/transparent_hugepage/defrag always || true
    echo "  transparent huge pages: always"

    # Disable NUMA balancing
    _athena_system_set /proc/sys/kernel/numa_balancing 0 || true
    echo "  NUMA balancing: disabled"

    echo "[launch-athena] system configured."
}

restore_system_performance() {
    echo "  restoring system to normal..."

    # Hand the power-profile manager back (restart PPD / restore tuned profile).
    local manager_restore_attempted=false
    if $PPD_WAS_ACTIVE; then
        manager_restore_attempted=true
        if sudo systemctl start power-profiles-daemon 2>/dev/null; then
            echo "    power-profiles-daemon: restarted"
            PPD_WAS_ACTIVE=false
        fi
    fi
    if [ -n "${TUNED_SAVE:-}" ]; then
        manager_restore_attempted=true
        if sudo tuned-adm profile "$TUNED_SAVE" 2>/dev/null; then
            echo "    tuned: restored to $TUNED_SAVE"
            TUNED_SAVE=""
        fi
    fi
    # Restore the manager/profile first: either may itself write governors,
    # EPP or the platform profile. Then restore the actual values captured
    # before setup, rather than letting a guessed manager default win.
    # An early failure before setup and repeated cleanup have no writes here.
    if $manager_restore_attempted; then _athena_system_observe_changes; fi
    _athena_system_restore
    if ((${#ATHENA_SYSTEM_CHANGED[@]})); then
        echo "    system controls: some saved values could not be restored; retained for cleanup retry"
    else
        echo "    system controls: restored successfully changed settings to their saved values"
    fi

}

# ── Cleanup function ──────────────────────────────────────────────────────────

# ── Audio routing ─────────────────────────────────────────────────────────────
# Streams are pinned per-process via PULSE_SOURCE / PULSE_SINK exported to
# the children (SDL capture and aplay both honor them), NOT by flipping
# system defaults. Field finding (pactl dump, 2026-06-12): default-flipping
# silently failed — pipewire registers the AEC nodes asynchronously, so
# set-default raced them (error swallowed by 2>/dev/null), and WirePlumber's
# stream-restore re-routed aplay / talk-llama to their remembered raw
# devices anyway. Both streams ran on the bare headset the whole time while
# the AEC module sat loaded and IDLE. An explicit per-stream target beats
# stream-restore and cannot land on an unregistered node.
#
#   ATHENA_AUDIO=direct  (default) mic and playback on the bare headset.
#                        With a headset, physical isolation replaces echo
#                        cancellation — no DSP in the path. Validated by the
#                        2026-06-11 20:11 field session (6 clean barges).
#   ATHENA_AUDIO=aec     route both streams through module-echo-cancel
#                        (webrtc). For open speakers + mic, where her voice
#                        would pollute the VAD floor and barge detector.
ATHENA_AUDIO="${ATHENA_AUDIO:-direct}"
HEADSET_PATTERN="${HEADSET_PATTERN:-Logi_USB_Headset}"
AEC_MODULE_ID=""

setup_audio() {
    if ! command -v pactl &>/dev/null; then
        echo "[launch-athena] WARNING: pactl not found (sudo apt install pulseaudio-utils) — using system default audio devices"
        return
    fi

    if [ "$ATHENA_AUDIO" = "aec" ]; then
        echo "[launch-athena] audio: echo-cancel mode"
        AEC_MODULE_ID=$(pactl load-module module-echo-cancel aec_method=webrtc \
            source_name=athena_aec_src sink_name=athena_aec_sink) || AEC_MODULE_ID=""
        if [ -n "$AEC_MODULE_ID" ]; then
            # Capture and playback nodes register independently. Require both
            # exact names before targeting either; a similarly named stale
            # source or an early source cannot stand in for the playback sink.
            # Read each complete listing so pipefail cannot treat a producer's
            # SIGPIPE after an early grep match as failed readiness.
            local i
            for i in $(seq 1 40); do
                if pactl list short sources | awk '$2 == "athena_aec_src" {found=1} END {exit !found}' &&
                   pactl list short sinks | awk '$2 == "athena_aec_sink" {found=1} END {exit !found}'; then
                    export PULSE_SOURCE=athena_aec_src
                    export PULSE_SINK=athena_aec_sink
                    echo "  mic      <- athena_aec_src (webrtc echo cancel)"
                    echo "  playback -> athena_aec_sink"
                    return
                fi
                sleep 0.05
            done
            echo "  WARNING: AEC nodes never registered — falling back to direct"
            pactl unload-module "$AEC_MODULE_ID" 2>/dev/null || true
            AEC_MODULE_ID=""
        else
            echo "  WARNING: module-echo-cancel failed to load — falling back to direct"
        fi
    fi

    echo "[launch-athena] audio: direct mode"
    local sink src
    sink=$(pactl list short sinks   | awk -v p="$HEADSET_PATTERN" '$2 ~ p {print $2; exit}')
    src=$(pactl  list short sources | awk -v p="$HEADSET_PATTERN" '$2 ~ p && $2 !~ /\.monitor$/ {print $2; exit}')
    if [ -n "$sink" ] && [ -n "$src" ]; then
        export PULSE_SINK="$sink"
        export PULSE_SOURCE="$src"
        echo "  playback -> $sink"
        echo "  mic      <- $src"
    else
        echo "  WARNING: no devices matching '$HEADSET_PATTERN' — using system defaults. Available:"
        pactl list short sinks   | awk '{print "    sink   " $2}'
        pactl list short sources | awk '$2 !~ /\.monitor$/ {print "    source " $2}'
    fi
}

restore_audio() {
    if [ -n "$AEC_MODULE_ID" ]; then
        pactl unload-module "$AEC_MODULE_ID" 2>/dev/null && echo "  echo cancel: unloaded"
        AEC_MODULE_ID=""
    fi
}

# ── r24.17 supervised signal and descendant ownership ───────────────────────
# r24.20 review (INTEGRATION F1): the shutdown is shaped around one fact the
# r24.17 launcher did not have — the brain's exit consolidation is the LONGEST
# thing a session does, and it is the thing the session exists for. S22
# measured it at 430 s (S1, 20:02:29 → 20:09:39) and 446 s (S2): two extractor
# passes, a cluster compaction and a personality revision on the 397B model,
# with stderr silent for up to 149 s between stages. The old "~20-30s" grace
# was a stale comment made load-bearing. The launcher's job is not to bound
# that pass; it is to outlive it. Hence:
#   1. the first signal forwards ONE SIGTERM to the brain immediately, before
#      any echo can fail on a dead log pipe, and marks it sent so no later
#      stop path can send a second one (the brain's second signal is SIG_DFL —
#      instant death, C49);
#   2. cleanup then waits for the brain with NO fixed deadline while the brain
#      shows progress (diag-log growth or CPU time, sampled every second),
#      narrating the wait to the terminal and to athena-diag.log;
#   3. the operator's SECOND Ctrl+C is the explicit escape (unchanged), and a
#      brain with no progress at all for ATHENA_SHUTDOWN_STALL_S (900 s, six
#      times the longest silent gap S22 measured; 0 = never) is treated as
#      wedged — MPS detach, the brain's own second-signal exit, then SIGKILL;
#   4. SIGPIPE is ignored from the brain launch onward (the launcher's own
#      cleanup chatter under `| ts | tee` could kill it before it signalled
#      the brain; the brain inherits the same and cannot die of a token
#      printed into the dead athena.log pipe while it notices its stop);
#   5. SIGHUP (the terminal window closed) is a stop like INT/TERM, instead of
#      killing the launcher and orphaning a 161 GiB brain in its own session.
# ATHENA_SHUTDOWN_WAIT_PROGRESS=0 restores the r24.20 fixed deadline
# (ATHENA_SHUTDOWN_GRACE_DS, default 60 s, then MPS detach + SIGKILL). 1–5 hold
# in both settings; they are defect repairs, not policy.
ATHENA_LAUNCH_OWNER_PID=$BASHPID
ATHENA_SHUTDOWN_WAIT_PROGRESS="${ATHENA_SHUTDOWN_WAIT_PROGRESS:-1}"
ATHENA_SHUTDOWN_STALL_S="${ATHENA_SHUTDOWN_STALL_S:-900}"
ATHENA_SHUTDOWN_NARRATE_S="${ATHENA_SHUTDOWN_NARRATE_S:-30}"
# "Busy" = at least this many CPU clock ticks per second (CLK_TCK is 100: 20 =
# a fifth of one core). The extractor runs the routed experts on 16 CPU threads
# (--cpu-moe) and even a GPU-only stage keeps a driver thread spinning, so a
# working brain measures in the hundreds; a brain whose exit pass is deadlocked
# still shows its SDL capture callback (audio.pause() comes after the pass) at
# well under one tick a second, which a plain "did the counter move" test would
# have mistaken for life. Diag output counts as progress on its own.
ATHENA_SHUTDOWN_BUSY_TICKS="${ATHENA_SHUTDOWN_BUSY_TICKS:-20}"
ATHENA_BRAIN_STOP_SENT=0
ATHENA_ENCODER_FORCE_REQUEST=0
ATHENA_STOP_STATUS=0      # the first trapped signal's exit status, read after `wait`

# One line, both to the terminal (which may already be gone — SIGPIPE is
# ignored, a failed write is a failed write) and, timestamped, to the diag log
# beside the brain's own consolidation lines, where the record of a shutdown
# that outlived its terminal is read afterwards.
_shutdown_say() {
    echo "[launch-athena] $*"
    [ -n "${ATHENA_DIAG_LOG:-}" ] && \
        printf '[%s] [launch-athena] %s\n' "$(date '+%Y-%m-%d %H:%M:%S.%6N')" "$*" >> "$ATHENA_DIAG_LOG" 2>/dev/null
    return 0
}

# Forward the one orderly stop to the tracked brain. Idempotent: the brain's
# SECOND signal is its C49 force-exit, so every stop path goes through here
# and the SIGTERM is sent exactly once per session.
_athena_brain_stop() {
    [ -n "$PID_TALK" ] || return 0
    [ "$TALK_REAPED" = 1 ] && return 0
    [ "$ATHENA_BRAIN_STOP_SENT" = 1 ] && return 0
    ATHENA_BRAIN_STOP_SENT=1
    kill -TERM "$PID_TALK" 2>/dev/null || true
}

_athena_signal() {
    local status="$1"
    # errexit off for everything that follows a stop signal. Measured (bash
    # 5.2.21): a SIGINT trap FUNCTION that runs while `set -e` is in force at
    # the entry of the EXIT trap — before cleanup's own `set +e` — aborts the
    # EXIT trap with "pop_var_context: head of shell_variables not a function
    # context", leaving the launcher hung and the teardown unrun. r24.20 never
    # hit it only because its brain signal came late enough that a second
    # Ctrl+C always landed inside cleanup. Nothing after a stop signal is a
    # step that errexit should guard; cleanup drops it anyway.
    set +e
    # A trap can run between the asynchronous command and PID_TALK=$!. Defer
    # exit across that tiny handoff so cleanup never loses a launched brain.
    if [ "$ATHENA_TALK_STARTING" = 1 ]; then
        if [ "$ATHENA_PENDING_SIGNAL" = 0 ]; then
            ATHENA_PENDING_SIGNAL=$status
        else
            ATHENA_FORCE_STOP=1
        fi
        return 0
    fi
    if [ "$ATHENA_CLEANING" = 1 ] || [ "$ATHENA_BRAIN_STOP_SENT" = 1 ]; then
        # A deliberate second user signal requests the existing forced-shutdown
        # backstop. It does not re-enter cleanup or accidentally double-signal a
        # brain that is still saving. MPS detach remains before any forced kill.
        # The latch is "the brain's stop has been sent", not "cleanup has
        # begun": the stop now goes out from this handler, before exit reaches
        # cleanup, and a second signal in that gap is still the second signal.
        ATHENA_FORCE_STOP=1
        _shutdown_say "second stop signal: forcing the remaining shutdown (the brain's consolidation will be cut)." >&2
        return
    fi
    # The brain first, silently: its consolidation starts now, and nothing the
    # launcher prints or stops from here on can lose it the signal.
    _athena_brain_stop
    # No `exit` from inside the trap. The r24.17 handler exited here, and a
    # second signal landing while that exit was still unwinding ran this
    # handler NESTED; when it returned, bash dropped the first exit and resumed
    # the interrupted `wait` — the launcher then sat through the brain's whole
    # pass with the operator's force request ignored (measured with xtrace
    # under the r24.17 fixture's immediate second Ctrl+C). The main line's
    # `wait` returns >128 the moment a trapped signal arrives; it reads this
    # status and exits itself, at top level, where the EXIT trap is reliable.
    ATHENA_STOP_STATUS=$status
    return 0
}

# Private parent/child contract for a terminal encoder deadline. The brain
# has already saved memory, but still owns an active worker/backend. It asks
# this supervisor to perform the same MPS-detach-before-force ordering as an
# explicit second interrupt; it must never exit its GPU process first.
_athena_encoder_timeout() {
    set +e
    [ -n "$PID_TALK" ] && [ "$TALK_REAPED" != 1 ] || return 0
    ATHENA_ENCODER_FORCE_REQUEST=1
    # This request is not an operator's deliberate forced stop. Its MPS
    # membership and successful detach must be established before any signal.
    # Do not send an orderly TERM before detach: an earlier external signal
    # might already have set the brain's stop flag, making that TERM fatal.
    ATHENA_BRAIN_STOP_SENT=1
    [ "$ATHENA_STOP_STATUS" != 0 ] || ATHENA_STOP_STATUS=1
    _shutdown_say "brain requested coordinated encoder shutdown; detaching MPS before termination." >&2
    return 0
}

# The MPS command reports PIDs in the server's namespace. Refuse automatic
# targeting if /proc is mismounted, a relevant namespace differs, or the
# reported server is not visibly an MPS process. Explicit operator-force
# behavior below is unchanged; uncertain automatic requests keep waiting.
_athena_mps_namespace_matches() {
    local client="$1" server="$2" stat_line="" comm="" ours theirs child
    read -r stat_line < /proc/self/stat 2>/dev/null || return 1
    [ "${stat_line%% *}" = "$BASHPID" ] || return 1
    read -r comm < "/proc/$server/comm" 2>/dev/null || return 1
    [[ "$comm" == nvidia-cuda-mps* ]] || return 1
    ours="$(readlink /proc/self/ns/pid 2>/dev/null)" || return 1
    theirs="$(readlink "/proc/$server/ns/pid" 2>/dev/null)" || return 1
    child="$(readlink "/proc/$client/ns/pid" 2>/dev/null)" || return 1
    [ -n "$ours" ] && [ "$ours" = "$theirs" ] && [ "$ours" = "$child" ]
}

# Strict automatic teardown uses the documented synchronous terminate_client
# reply: command success AND one numeric CUDA_SUCCESS (0). A successful shell
# invocation with CUDA error output is not a successful detach. Reused servers
# are supported per owned client; this helper never quits any MPS server.
# NVIDIA: docs.nvidia.com/deploy/mps/latest/when-to-use-mps.html#client-early-termination
_athena_mps_auto_detach() {
    local pid="$1" servers clients server client reply matched=0 count=0
    local -a targets=()
    if [ "${MPS_STARTED:-unknown}" = false ] && [ "${ATHENA_MPS:-unknown}" = 0 ] &&
       [ -z "${CUDA_MPS_PIPE_DIRECTORY:-}" ]; then
        _shutdown_say "encoder shutdown: MPS is disabled for this launcher."
        return 0
    fi
    command -v nvidia-cuda-mps-control >/dev/null 2>&1 || return 1
    command -v timeout >/dev/null 2>&1 || return 1
    servers="$(printf 'get_server_list\n' | timeout --kill-after=2s 5s nvidia-cuda-mps-control 2>/dev/null)" || return 1
    [[ "$servers" =~ ^[[:space:]0-9]+$ ]] || return 1
    for server in $servers; do
        [[ "$server" =~ ^[1-9][0-9]{0,9}$ ]] || return 1
        count=$((count+1)); [ "$count" -le 32 ] || return 1
        [ "$ATHENA_FORCE_STOP" != 1 ] || return 1
        _athena_mps_namespace_matches "$pid" "$server" || return 1
        clients="$(printf 'get_client_list %s\n' "$server" | timeout --kill-after=2s 5s nvidia-cuda-mps-control 2>/dev/null)" || return 1
        [[ "$clients" =~ ^[[:space:]0-9]+$ ]] || return 1
        matched=0
        for client in $clients; do
            [[ "$client" =~ ^[1-9][0-9]{0,9}$ ]] || return 1
            [ "$client" != "$pid" ] || matched=1
        done
        [ "$matched" != 1 ] || targets+=("$server")
    done
    [ "${#targets[@]}" -gt 0 ] || return 1
    for server in "${targets[@]}"; do
        [ "$ATHENA_FORCE_STOP" != 1 ] || return 1
        reply="$(printf 'terminate_client %s %s\n' "$server" "$pid" | timeout --kill-after=2s 5s nvidia-cuda-mps-control 2>/dev/null)" || return 1
        [[ "$reply" =~ ^[[:space:]]*0[[:space:]]*$ ]] || return 1
    done
    _shutdown_say "encoder shutdown: MPS confirmed detach for the owned client on ${#targets[@]} server(s)."
    return 0
}

# The brain's liveness as the launcher can observe it, into two globals:
# ATHENA_BRAIN_STATE = "" when gone (no /proc entry, or a zombie awaiting our
# wait()), else the /proc state letter; ATHENA_BRAIN_TICKS = its CPU ticks.
# Builtins only, no command substitution: a trapped signal (the operator's
# second Ctrl+C) landing inside a `$(cat …)` kills that subshell and hands
# back an empty string — which, read as "gone", skipped the forced stop and
# left the launcher waiting out the whole pass (measured in the r24.17
# fixture). A read that fails while /proc/<pid> still exists reports the brain
# as present and idle, never as gone.
ATHENA_BRAIN_STATE=""; ATHENA_BRAIN_TICKS=0
_athena_brain_probe() {
    local s="" pid="$1"
    ATHENA_BRAIN_STATE=""; ATHENA_BRAIN_TICKS=0
    # The signal namespace owns the PID. A differently mounted /proc may have
    # no row for it, or a row for an unrelated host process with the same PID.
    kill -0 "$pid" 2>/dev/null || return 0
    read -r s < /proc/self/stat 2>/dev/null || s=""
    if [ "${s%% *}" != "$BASHPID" ]; then
        ATHENA_BRAIN_STATE="?"
        return 0
    fi
    # Restricted proc mounts / PID namespaces can hide an owned live child.
    # kill -0 is the liveness fallback; unavailable CPU telemetry is unknown,
    # never evidence that a still-running consolidation has already exited.
    if [ ! -d "/proc/$pid" ]; then
        kill -0 "$pid" 2>/dev/null && ATHENA_BRAIN_STATE="?"
        return 0
    fi
    read -r s < "/proc/$pid/stat" 2>/dev/null || read -r s < "/proc/$pid/stat" 2>/dev/null || s=""
    if [ -z "$s" ]; then
        kill -0 "$pid" 2>/dev/null && ATHENA_BRAIN_STATE="?"
        return 0
    fi
    s="${s##*) }"                 # past "pid (comm) " — comm may hold spaces
    set -- $s                     # $1 = state, $12 = utime, $13 = stime
    [ "${1:-Z}" = Z ] && return 0
    ATHENA_BRAIN_STATE="$1"; ATHENA_BRAIN_TICKS=$(( ${12:-0} + ${13:-0} ))
    return 0
}

# Wait for the brain's exit pass. Returns when the brain is gone (normally,
# forced, or already reaped). Progress = the diag log grew, or the brain was
# busy (ATHENA_SHUTDOWN_BUSY_TICKS of CPU) over the last one-second sample; the
# 397B extractor burns CPU throughout its 149-s silent stretches, so a brain
# that is both silent and idle for ATHENA_SHUTDOWN_STALL_S is a wedged one. A
# spinning wedge with no output would wait on the operator's second Ctrl+C —
# the cheaper failure: a false kill loses the session, a false wait is visible.
# The diag-log size is the one external read left; a substitution the second
# Ctrl+C kills yields "", which is folded into "no change" — never progress,
# never gone.
_athena_log_size() {
    local v
    v="$(stat -c %s -- "$ATHENA_DIAG_LOG" 2>/dev/null)" || v=""
    [ -n "$v" ] && echo "$v" || echo "$1"
}
_athena_wait_brain() {
    local pid="$PID_TALK" ticks last_ticks=-1 size last_size=0
    local t0 now last_progress narrated_at elapsed since forced=0 busy=no
    local auto_attempted=0 auto_detached=0
    [ -n "$pid" ] || return 0
    [ "$TALK_REAPED" = 1 ] && return 0
    _athena_brain_probe "$pid"
    [ -n "$ATHENA_BRAIN_STATE" ] || return 0
    _athena_brain_stop
    t0=$SECONDS; last_progress=$t0; narrated_at=$t0
    _shutdown_say "waiting for the brain ($pid) to finish its exit consolidation — S22 measured 7 min; press Ctrl+C again to force"
    # Our own narration lands in the same log: re-read the size after every
    # line of ours so the launcher cannot mistake itself for a brain that is
    # still alive (the harness's wedge case caught exactly that).
    last_size="$(_athena_log_size 0)"
    while :; do
        _athena_brain_probe "$pid"
        [ -n "$ATHENA_BRAIN_STATE" ] || break
        if [ "$ATHENA_FORCE_STOP" = 1 ]; then forced=1; break; fi
        if [ "$ATHENA_ENCODER_FORCE_REQUEST" = 1 ] && [ "$auto_attempted" = 0 ]; then
            auto_attempted=1
            if _athena_mps_auto_detach "$pid"; then
                auto_detached=1; forced=1; break
            fi
            _shutdown_say "encoder shutdown: automatic MPS detach was not confirmed; retaining the live worker and waiting. Press Ctrl+C again for an explicit forced stop."
        fi
        ticks="$ATHENA_BRAIN_TICKS"
        size="$(_athena_log_size "$last_size")"
        now=$SECONDS
        busy=no
        if [ "$last_ticks" -ge 0 ] && [ $((ticks - last_ticks)) -ge "$ATHENA_SHUTDOWN_BUSY_TICKS" ] 2>/dev/null; then busy=yes; fi
        if [ "$busy" = yes ] || [ "$size" != "$last_size" ]; then last_progress=$now; fi
        last_ticks=$ticks; last_size=$size
        elapsed=$((now - t0)); since=$((now - last_progress))
        if [ "$ATHENA_ENCODER_FORCE_REQUEST" != 1 ] && [ "$ATHENA_SHUTDOWN_STALL_S" -gt 0 ] 2>/dev/null && [ "$since" -ge "$ATHENA_SHUTDOWN_STALL_S" ]; then
            _shutdown_say "no progress from the brain for ${since}s (no diag output, CPU idle) — treating it as wedged; forcing"
            forced=1; break
        fi
        if [ $((now - narrated_at)) -ge "$ATHENA_SHUTDOWN_NARRATE_S" ] 2>/dev/null; then
            _shutdown_say "brain still consolidating: ${elapsed}s elapsed, last progress ${since}s ago, CPU $([ "$busy" = yes ] && echo busy || echo idle) (Ctrl+C again to force)"
            narrated_at=$now
            last_size="$(_athena_log_size "$last_size")"
        fi
        sleep 1
    done
    _athena_brain_probe "$pid"
    if [ "$forced" = 1 ] && [ -n "$ATHENA_BRAIN_STATE" ]; then
        _shutdown_say "forcing the brain ($pid): MPS detach, then its own second-signal exit, then SIGKILL"
        echo "           (a forced GPU-client teardown under MPS can corrupt kernel page state;"
        echo "            terminate_client first drains the server's per-client worker thread)"
        if [ "$auto_detached" != 1 ]; then
            mps_terminate_client "$pid"   # explicit operator force keeps its historical best-effort policy
        fi
        kill -TERM "$pid" 2>/dev/null # the brain's C49 second signal: SIG_DFL + raise
        local i
        for ((i=0; i<50; i++)); do
            _athena_brain_probe "$pid"
            [ -n "$ATHENA_BRAIN_STATE" ] || break
            sleep 0.1
        done
        _athena_brain_probe "$pid"
        [ -n "$ATHENA_BRAIN_STATE" ] && kill -KILL "$pid" 2>/dev/null
    fi
    _shutdown_say "brain ($pid) has exited after $((SECONDS - t0))s"
    return 0
}

_athena_drain_brain_group() {
    local i
    # The brain's normal teardown owns its workers. Only after it is gone do we
    # terminate leftover external descendants (for example a stalled capture).
    # A separate process group makes this target installation-owned, unlike pkill.
    if [ -n "$PGID_TALK" ]; then
        if kill -0 -- "-$PGID_TALK" 2>/dev/null; then
            kill -TERM -- "-$PGID_TALK" 2>/dev/null || true
            for ((i=0; i<20; i++)); do
                kill -0 -- "-$PGID_TALK" 2>/dev/null || break
                [ "$ATHENA_FORCE_STOP" = 1 ] && break
                sleep 0.1
            done
            kill -0 -- "-$PGID_TALK" 2>/dev/null && kill -KILL -- "-$PGID_TALK" 2>/dev/null
        fi
        PGID_TALK=""
    fi
    # Parent closes its write FD immediately after launch. Once descendants are
    # gone the timestamper sees EOF. Drain it before the evidence helper can take
    # the final log boundary; a failed logger is explicit, with bounded waiting.
    if [ -n "$PID_TALK_LOG" ]; then
        for ((i=0; i<50; i++)); do
            kill -0 "$PID_TALK_LOG" 2>/dev/null || break
            [ "$ATHENA_FORCE_STOP" = 1 ] && break
            sleep 0.1
        done
        if kill -0 "$PID_TALK_LOG" 2>/dev/null; then
            echo "[launch-athena] WARNING: brain log writer did not drain; terminating it." >&2
            kill -TERM "$PID_TALK_LOG" 2>/dev/null
            for ((i=0; i<10; i++)); do
                kill -0 "$PID_TALK_LOG" 2>/dev/null || break
                sleep 0.1
            done
            kill -0 "$PID_TALK_LOG" 2>/dev/null && kill -KILL "$PID_TALK_LOG" 2>/dev/null
        fi
        wait "$PID_TALK_LOG" 2>/dev/null || true
        PID_TALK_LOG=""
    fi
}
# ── end r24.17 supervised signal and descendant ownership ───────────────────

cleanup() {
    # A TERM can reach an asynchronous external-command child before Bash
    # finishes resetting inherited traps. Its EXIT trap then sees a private
    # copy of ATHENA_CLEANING=0. Only this launcher owns process teardown and
    # restoration; a forked shell must never act on the parent's resources.
    [ "$BASHPID" = "$ATHENA_LAUNCH_OWNER_PID" ] || return 0
    if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
        [ "$ATHENA_CLEANING" = 1 ] && return
        ATHENA_CLEANING=1
    fi
    set +e     # Don't exit on error during cleanup
    # r24.20 review (F1 a): under `| ts | tee` the terminal's Ctrl+C has
    # already killed this script's stdout reader; an `echo` below would
    # SIGPIPE the launcher out of its own teardown (MPS left up, clocks locked,
    # audio module loaded). Both arms — the supervised one set this at launch.
    trap '' PIPE
    trap - ERR # `set +e` stops errexit but NOT the ERR trap (it is inherited via
               # `set -E`, line 18), so every EXPECTED non-zero return in teardown
               # (bounded MPS `quit` timing out = 124, pkill matching nothing = 1)
               # printed a spurious "[launch-athena] ERROR at line N" — the
               # misleading line-382/387 noise in the 230402 shutdown. Drop it here;
               # the global trap (line 19) still catches a REAL crash before cleanup.
    echo ""
    echo "[launch-athena] shutting down..."

    # Stop the diagnostics sampler last-started/first-stopped, but give it a beat
    # so it records the shutdown (and any abort the kills below surface). The
    # monitor traps TERM and tears down its own nvidia-smi/dmesg children.
    if [ -n "$PID_GPUMON" ] && kill -0 "$PID_GPUMON" 2>/dev/null; then
        kill "$PID_GPUMON" 2>/dev/null
        echo "  stopped GPU/Xid sampler ($PID_GPUMON)"
    fi
    [ -n "$RUN_DIR" ] && [ -d "$RUN_DIR" ] && echo "  diagnostics saved to $RUN_DIR"

    # Stop the llama-server watchdog + keepalive FIRST so they cannot respawn/poke the
    # backend mid-teardown. The sentinel blocks an in-flight respawn; TERM trips their
    # traps (exit 0), which deliberately leave llama-server for the ordered _stop_pid
    # below — preserving the load-bearing kill ORDER (orpheus → llama → talk-llama).
    local _wp _p _i
    touch "$LLAMA_WD_STOP" 2>/dev/null
    for _wp in PID_WATCHDOG PID_KEEPALIVE PID_ORPHWD; do
        _p="${!_wp}"
        if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
            kill "$_p" 2>/dev/null
            for _i in $(seq 1 20); do kill -0 "$_p" 2>/dev/null || break; sleep 0.1; done
            kill -0 "$_p" 2>/dev/null && kill -9 "$_p" 2>/dev/null
            echo "  stopped $_wp ($_p)"
        fi
    done

    # ── Ordered teardown (avoids the MPS page-accounting kernel crash) ─────────
    # We hit a kernel "BUG: Bad page state ... page still charged to cgroup" in
    # exit_mmap during shutdown: a GPU client's address space was torn down while
    # GPU work was still in flight under MPS, and the driver left a mapped/pinned
    # page mis-accounted, corrupting kernel memory and locking the machine.
    # talk-llama runs its exit "memory consolidation" on the 397B model at exit
    # — S22 measured 430 s and 446 s (r24.20 review, INTEGRATION F1; the
    # "~20-30s" that used to stand here was never measured and sized a 60-s
    # SIGKILL that would have lost every Ctrl+C-ended session's memories) — so
    # it MUST stop LAST and be allowed to finish on its own: a SIGKILL
    # mid-generation, with its ~161 GiB pinned host allocation, is exactly the
    # forced teardown that corrupts page state. The auxiliary GPU clients
    # (orpheus-speak, llama-server) are idle once the goodbye is spoken, so stop
    # them FIRST and let them fully release their MPS contexts while the brain
    # is still generating; its own teardown comes minutes later, alone.
    _stop_pid() {                 # $1 = pid var name   $2 = grace in 0.1s units
        local pid="${!1}" grace="$2" i
        [ -z "$pid" ] && return 0
        if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
            [ "$1" = PID_TALK ] && [ "$TALK_REAPED" = 1 ] && return 0
            # Do not let malformed shell arithmetic turn an empty or negative
            # grace into immediate SIGKILL; unset/invalid restores sixty seconds.
            if ! [[ "$grace" =~ ^[1-9][0-9]{0,5}$ ]]; then grace=600; fi
        fi
        kill -0 "$pid" 2>/dev/null || return 0
        # r24.20 review (F1): the brain's stop is sent exactly once, by
        # _athena_brain_stop — a second SIGTERM is its C49 instant exit.
        if [ "$1" = PID_TALK ] && [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
            _athena_brain_stop
        else
            kill "$pid" 2>/dev/null
        fi
        for ((i=0; i<grace; i++)); do
            kill -0 "$pid" 2>/dev/null || break
            if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ] && [ "$ATHENA_FORCE_STOP" = 1 ]; then break; fi
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            echo "  WARNING: $1 ($pid) still alive after $((grace/10))s — detaching MPS context, then SIGKILL"
            echo "           (a forced GPU-client teardown under MPS can corrupt kernel page state;"
            echo "            terminate_client first drains the server's per-client worker thread)"
            mps_terminate_client "$pid"   # documented graceful detach before the kill (no-op without MPS)
            kill -9 "$pid" 2>/dev/null
        fi
        echo "  stopped $1 ($pid)"
    }

    # The orpheus watchdog may have restarted the daemon → live PID differs from launch.
    [ -r "$ORPHEUS_PIDFILE" ] && PID_ORPHEUS=$(cat "$ORPHEUS_PIDFILE" 2>/dev/null)
    _stop_pid PID_ORPHEUS 50      # 5s  — TTS playback finished after the goodbye
    # The watchdog may have restarted llama-server → the live PID differs from launch.
    [ -r "$LLAMA_PIDFILE" ] && PID_LLAMA=$(cat "$LLAMA_PIDFILE" 2>/dev/null)
    _stop_pid PID_LLAMA   50      # 5s  — Orpheus server not used by consolidation
    # talk-llama last. Supervised: wait for its exit pass by PROGRESS, not by a
    # deadline (r24.20 review, F1 — see _athena_wait_brain); the operator's
    # second Ctrl+C or a genuine stall is what ends the wait early.
    # ATHENA_SHUTDOWN_WAIT_PROGRESS=0, or the foreground arm: the r24.20 fixed
    # grace (60 s default), then MPS detach + SIGKILL.
    if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ] && { [ "$ATHENA_SHUTDOWN_WAIT_PROGRESS" != "0" ] || [ "$ATHENA_ENCODER_FORCE_REQUEST" = 1 ]; }; then
        _athena_wait_brain
    else
        _stop_pid PID_TALK "${ATHENA_SHUTDOWN_GRACE_DS:-600}"   # 60s default
    fi
    if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
        [ -n "$PID_TALK" ] && [ "$TALK_REAPED" != 1 ] && wait "$PID_TALK" 2>/dev/null
        TALK_REAPED=1
        _athena_drain_brain_group
    fi

    # Remove temp files
    rm -f "$SPEAK_FILE" "$TRIGGER_FILE" "$DONE_FILE" "$STOP_FILE" "$WAV_FILE" \
          "$LLAMA_PIDFILE" "$ORPHEUS_PIDFILE" "$LLAMA_WEDGE" "$LLAMA_WD_STOP"
    echo "  cleaned up temp files"

    # Restore the kernel core_pattern if the core-dump setup changed it.
    if [ -n "$CORE_PATTERN_SAVE" ]; then
        printf '%s\n' "$CORE_PATTERN_SAVE" | sudo tee /proc/sys/kernel/core_pattern >/dev/null 2>&1 \
            && echo "  core_pattern: restored"
    fi

    # Restore audio routing, system and GPU to pre-launch state. MPS is stopped
    # after the GPU client processes above are down and before the GPU clocks /
    # power limit are reverted.
    restore_audio
    restore_system_performance
    stop_mps
    restore_gpu_performance

    # r24.20 review (F1): also into the diag log — under `| ts | tee` the
    # terminal that would have shown this line died with the Ctrl+C minutes ago.
    _shutdown_say "done."
}

if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
    trap cleanup EXIT
    trap '_athena_signal 130' INT
    trap '_athena_signal 143' TERM
    trap '_athena_encoder_timeout' USR1
    # r24.20 review (F1 b): a closed terminal window is a stop, not an orphaning.
    # The brain lives in its own session and never sees the terminal's HUP; the
    # launcher did, with the default disposition — dead launcher, 161 GiB brain
    # still listening, MPS up, clocks locked, and nothing left to signal it.
    if [[ "$(trap -p HUP)" == *"''"* ]]; then
        echo "[launch-athena] SIGHUP was ignored at shell entry (for example nohup); hangup will not stop this launcher. Use TERM for a graceful stop." >&2
    fi
    trap '_athena_signal 129' HUP
else
    trap cleanup EXIT INT TERM
fi

# ── GPU performance tuning ────────────────────────────────────────────────────

setup_gpu_performance

# ── CUDA MPS (diagnostic, ATHENA_MPS=1) ───────────────────────────────────────
# Must run before any GPU process so llama-server, orpheus-speak and talk-llama
# all inherit the exported CUDA_MPS_* env and share one MPS context. No-op unless
# ATHENA_MPS=1.
start_mps

# ── CPU / system performance tuning ───────────────────────────────────────────

setup_system_performance

# ── Audio routing ──────────────────────────────────────────────────────────────

setup_audio

# ── Preflight checks ──────────────────────────────────────────────────────────

check_file() {
    if [ ! -f "$1" ]; then
        echo "[launch-athena] ERROR: $2 not found: $1"
        exit 1
    fi
}

check_file "$LLAMA_SERVER"  "llama-server binary"
check_file "$ORPHEUS_SPEAK" "orpheus-speak binary"
check_file "$TALK_LLAMA"    "whisper-talk-llama binary"
check_file "$ORPHEUS_MODEL" "Orpheus GGUF model"
check_file "$SNAC_MODEL"    "SNAC ONNX decoder"
# Qwen is a split GGUF — llama.cpp opens shard 1 and pulls in the rest, so a
# missing later shard would otherwise only fail minutes into the load.
for i in $(seq 1 "$QWEN_SHARDS"); do
    check_file "${QWEN_MODEL/00001-of/0000${i}-of}" "Qwen3.5 GGUF shard ${i}/${QWEN_SHARDS}"
done
check_file "$WHISPER_MODEL" "Whisper model"
check_file "$VAD_MODEL"     "Silero VAD model"
check_file "$SPEAK_DAEMON"  "speak-daemon.sh"

# ── r24.20 review (SPEECH F1 b): the daemon must speak the brain's protocol ──
# The r24.19 brain labels every TTS session (`---ATHENA_SESSION id S---`) and
# accepts only a receipt that carries that label. An orpheus-speak built from
# r24.14 or stock has no notion of the label: it speaks the header line ALOUD,
# indexes her lines one off, and writes a bare receipt the brain never matches
# — the brain then waits up to thirty minutes per reply, silently. That
# pairing is exactly what a partial rebuild produces (stock install.sh dies on
# the overlay's mtmd/ directory before rebuilding anything, and its
# --skip-whisper form skips the daemon build on a green ldd). Nothing at
# runtime checked the pair. Probe the daemon binary once: r24.20's
# `--trace-wrapper` entry point runs before any model, curl, SNAC or audio
# initialisation and exits 0 (with no arguments it writes nothing anywhere,
# whatever ATHENA_TTS_PROTOCOL_TRACE_DIR says); an older binary answers
# "Unknown option" and exits 1. Refuse to launch on a mismatch, unless
# ATHENA_TTS_SESSION_ID=0 is exported — the documented isolated legacy
# comparison, in which a bare receipt IS the protocol. ATHENA_DAEMON_PREFLIGHT=0
# restores r24.20 (no probe). Cost: one exec at startup.
ATHENA_DAEMON_PREFLIGHT="${ATHENA_DAEMON_PREFLIGHT:-1}"
if [ "$ATHENA_DAEMON_PREFLIGHT" != "0" ]; then
    if timeout --kill-after=2s 10s "$ORPHEUS_SPEAK" --trace-wrapper >/dev/null 2>&1; then
        echo "[launch-athena] orpheus-speak speaks the r24.19 session protocol (probe ok)"
    elif [ "${ATHENA_TTS_SESSION_ID:-1}" = "0" ]; then
        echo "[launch-athena] WARNING: orpheus-speak predates the r24.19 session protocol; continuing because ATHENA_TTS_SESSION_ID=0 (legacy comparison) is exported"
    else
        echo "[launch-athena] ERROR: orpheus-speak at $ORPHEUS_SPEAK does not speak the r24.19 session protocol" >&2
        echo "                (its --trace-wrapper probe failed or timed out). Against an old daemon the brain would speak" >&2
        echo "                its session header aloud and wait up to 30 minutes per reply for a receipt that never matches." >&2
        echo "                Rebuild it: cmake --build orpheus/build --target orpheus-speak  (INSTALL-CONSCIOUSNESS.md:" >&2
        echo "                brain, daemon and wrapper together). ATHENA_TTS_SESSION_ID=0 for all three is the isolated" >&2
        echo "                legacy comparison; ATHENA_DAEMON_PREFLIGHT=0 skips this check." >&2
        exit 1
    fi
fi
# ── end r24.20 daemon protocol preflight ─────────────────────────────────────

# Clean stale temp files from previous runs
rm -f "$SPEAK_FILE" "$TRIGGER_FILE" "$DONE_FILE" "$STOP_FILE"

# Remove stale ONNX optimized graph cache — a GPU-optimized cache will cause
# VRAM allocation even when running SNAC on CPU via --snac-cpu
rm -f "$OPT_CACHE"

# ── Terminal 1: llama-server (Orpheus TTS) ────────────────────────────────────

# ── Diagnostics: GPU / Xid telemetry sampler ──────────────────────────────────
# Start before the heavy model loads so VRAM growth, clock locks, and any load-time
# fault are captured. Best-effort: a missing script just skips it.
if [ "$ATHENA_DIAG" != "0" ] && [ -n "$RUN_DIR" ]; then
    if [ -x "$GPU_MONITOR" ]; then
        # 1000 ms cadence (was 500): halves the highest-rate nvidia-smi spawner — the
        # process that drew the "Bad page state" poisoned page on 20260701-123219 — at
        # near-zero diagnostic loss (clocks/power move slowly; fault capture is
        # event-driven via athena-kmsg-logger). Override: "$GPU_MONITOR" <dir> <ms>.
        "$GPU_MONITOR" "$RUN_DIR" "${ATHENA_GPU_SAMPLE_MS:-1000}" &
        PID_GPUMON=$!
        echo "[launch-athena] diagnostics -> $RUN_DIR (gpu/xid sampler pid $PID_GPUMON)"
    else
        echo "[launch-athena] NOTE: $GPU_MONITOR not executable — GPU sampler skipped (server log still captured)"
    fi
fi

echo "[launch-athena] starting llama-server (Orpheus TTS)..."

# Bench-verified across two runs (orpheus-bench.sh, 2026-06):
#   GGML_CUDA_GRAPH_OPT=1 -> no measurable benefit (run 2: 158.8 vs 159.7 off).
#     NOW UNSET (see CHANGES.MD §10): it is an *experimental* multi-stream/
#     buffer-reuse CUDA path (llama.cpp PR #16991, off-by-default, ~1-9% TG only
#     at batch-1) whose author flags a buffer-reuse race — exactly the class that
#     surfaces as intermittent "unspecified launch failure". Base CUDA graphs
#     (-DGGML_CUDA_GRAPHS=ON) + kernel fusion stay ON and self-disable at batch>1,
#     so retiring GRAPH_OPT retires extra concurrent streams at ~0 perf cost.
#   KV f16 vs q8_0 -> within power-hunting noise (each "won" one run by ~5%).
#     q8_0 now kept: bench-neutral, and frees ~0.75 GiB for the 397B's budget.
# Real bottleneck: ~94 W power wall collapses SM clock to ~870 MHz under
# sustained decode (see orpheus-powertune.sh). Single-stream ceiling ~160 tok/s.
# -np 2 (was 4): ATHENA is single-user, so np>1 gives ZERO per-stream latency
# benefit (Orpheus needs 82 tok/s/stream for real-time; single-stream is ~160).
# np only fans the client's sentence-chunk pipeline across slots. At np=2 each
# stream holds ~122 tok/s (1.49x RT) vs ~87 (1.07x) at np=4 — the real-time
# margin GROWS while peak concurrent-decode stress (and shared MPS/GSP stream
# count) HALVES. Per-slot ctx 13824/2=6912 still fits the largest chunk (~1,800
# tok). The 230402 fatal fault hit with 3 slots decoding concurrently.
# stdbuf -oL -eL forces line buffering so the final lines before an abort (the
# CUDA error / assertion / OOM reason) are flushed rather than lost in glibc's
# block buffer. Process substitution keeps $! = llama-server's PID for the health
# checks and cleanup below. With ATHENA_DIAG=0, ORPHEUS_SERVER_LOG is /dev/null.
# Core dumps for the SMALL GPU clients only (llama-server, orpheus-speak) so a libcuda
# GPF (20260701-123219) leaves a gdb-inspectable backtrace. NEVER talk-llama: its
# ~161 GiB mlock'd RSS would write a catastrophic core. (CHANGES.MD §12)
if [ "$ATHENA_COREDUMP" != "0" ] && [ -n "$RUN_DIR" ]; then
    CORE_PATTERN_SAVE="$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
    if printf '%s\n' "$RUN_DIR/core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern >/dev/null 2>&1; then
        CORE_ULIMIT=unlimited
        echo "[launch-athena] core dumps ON for llama-server/orpheus-speak -> $RUN_DIR/core.* (ATHENA_COREDUMP=0 to disable)"
    else
        CORE_PATTERN_SAVE=""   # nothing changed → nothing to restore
        echo "[launch-athena] NOTE: could not set core_pattern (need sudo) — core dumps off"
    fi
fi

# start_llama_server: launch llama-server EXACTLY as configured and record its live PID
# in PID_LLAMA and $LLAMA_PIDFILE (the values cleanup() and the watchdog trust). The
# ( ulimit -c …; exec … ) wrapper sets the core limit for THIS client only; exec keeps
# $! == llama-server's PID. stdbuf -oL -eL forces line buffering so the last lines
# before an abort survive; the process substitution preserves $! (see 230402 notes).
start_llama_server() {
    ( ulimit -c "$CORE_ULIMIT"
      exec stdbuf -oL -eL "$LLAMA_SERVER" \
          -m "$ORPHEUS_MODEL" \
          -c 13824 -np 2 -ngl 99 \
          --host 127.0.0.1 --port 8080 \
          --cache-type-k f16 --cache-type-v f16 --cache-ram 0 \
          -fa on -t 0
    ) > >(_ts >> "$ORPHEUS_SERVER_LOG") 2>&1 &
    PID_LLAMA=$!
    echo "$PID_LLAMA" > "$LLAMA_PIDFILE" 2>/dev/null
}

# llama_watchdog: background health-watchdog + auto-restart. Scoped to the userspace-
# death case (SIGSEGV/GPF/OOM in llama-server, GPU still healthy, MPS not poisoned) —
# exactly the 20260701-123219 failure. It CANNOT save a device Xid (that poisons all
# MPS clients and needs a full-stack relaunch); it caps restarts and gives up if so.
llama_watchdog() {
    set +e; trap - ERR
    trap 'exit 0' TERM INT
    local WD_LOG="${RUN_DIR:-/dev/shm}/watchdog.log"
    _wlog() { echo "[$(date '+%F %T.%3N')] watchdog[$BASHPID]: $*" >> "$WD_LOG" 2>/dev/null; }
    _wlog "armed — guarding llama-server :8080 (interval ${WD_INTERVAL}s, cap ${WD_MAX_RESTARTS}/${WD_WINDOW}s)"
    local -a stamps=()
    local fails=0 i t pid down cutoff ok backoff=2
    while :; do
        [ -f "$LLAMA_WD_STOP" ] && { _wlog "stop sentinel — exiting"; exit 0; }
        sleep "$WD_INTERVAL"
        [ -f "$LLAMA_WD_STOP" ] && { _wlog "stop sentinel — exiting"; exit 0; }
        pid=$(cat "$LLAMA_PIDFILE" 2>/dev/null); down=0
        if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
            down=1                                           # GPF/exit — definitive fast path
        elif [ -f "$LLAMA_WEDGE" ]; then
            rm -f "$LLAMA_WEDGE" 2>/dev/null
            down=1                                           # keepalive-flagged decode wedge (§20):
            _wlog "decode WEDGE flagged by keepalive (health green, completions dead) — forcing restart"
        elif ! curl -sf --max-time 2 http://127.0.0.1:8080/health >/dev/null 2>&1; then
            fails=$((fails+1)); [ "$fails" -ge "$WD_FAIL_THRESHOLD" ] && down=1   # wedged/half-alive
        else
            fails=0
        fi
        [ "$down" -eq 0 ] && continue
        fails=0
        cutoff=$(( SECONDS - WD_WINDOW ))
        local -a kept=()
        for t in "${stamps[@]}"; do [ "$t" -ge "$cutoff" ] && kept+=("$t"); done
        stamps=("${kept[@]}")
        if [ "${#stamps[@]}" -ge "$WD_MAX_RESTARTS" ]; then
            _wlog "GIVING UP: ${#stamps[@]} restarts within ${WD_WINDOW}s — backend hard-down (likely a device-level fault; TTS offline, brain unaffected)"
            exit 0
        fi
        stamps+=("$SECONDS")
        _wlog "llama-server DOWN (pid=${pid:-none}); restart #${#stamps[@]}/${WD_MAX_RESTARTS}"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then    # wedged: force it gone before rebind
            mps_terminate_client "$pid" 2>/dev/null
            kill -9 "$pid" 2>/dev/null
            for i in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        fi
        for i in $(seq 1 25); do curl -s --max-time 1 http://127.0.0.1:8080/health >/dev/null 2>&1 || break; sleep 0.2; done
        [ -f "$LLAMA_WD_STOP" ] && { _wlog "stop before respawn — exiting"; exit 0; }
        start_llama_server
        _wlog "respawned pid $PID_LLAMA — polling /health"
        ok=0
        for i in $(seq 1 120); do
            [ -f "$LLAMA_WD_STOP" ] && exit 0
            if curl -sf --max-time 2 http://127.0.0.1:8080/health >/dev/null 2>&1; then ok=1; break; fi
            kill -0 "$PID_LLAMA" 2>/dev/null || break        # died during load
            sleep 0.5
        done
        if [ "$ok" -eq 1 ]; then
            _wlog "HEALTHY again (pid $PID_LLAMA) — TTS restored; orpheus-speak reconnects on its next request"
            backoff=2
        else
            _wlog "respawn not healthy (pid $PID_LLAMA); backing off ${backoff}s"
            sleep "$backoff"; backoff=$(( backoff*2 )); [ "$backoff" -gt 30 ] && backoff=30
        fi
    done
}

# llama_keepalive: idle 1-token poke so the CUDA context/graphs never go fully cold
# (the 123219 GPF hit 13 ms after a 67 s-idle re-entry). ~8 ms GPU / 30 s; off the
# user-latency path. Harmless while llama-server is momentarily down (curl just fails).
llama_keepalive() {
    set +e; trap - ERR; trap 'exit 0' TERM INT
    local util kfail=0 csv="${RUN_DIR:+$RUN_DIR/gpu.csv}"
    while :; do
        [ -f "$LLAMA_WD_STOP" ] && exit 0
        sleep "${ATHENA_KEEPALIVE_INTERVAL:-30}"
        [ -f "$LLAMA_WD_STOP" ] && exit 0
        # IDLE-GATE (CHANGES.MD §15): poke ONLY when the GPU is idle. When busy the
        # CUDA context is already warm, so the poke buys nothing — and it injects a
        # SECOND MPS client's decode on top of the brain's. That concurrency-at-the-
        # power-wall pattern is present at both Xid-69 faults (074343: 3 TTS slots
        # decoding; 174424: the poke came due within ~100 ms of the Xid while the
        # brain decoded at 88% util on the 94 W cap). gpu.csv col 7 (utilization.gpu)
        # is refreshed ~1/s by the persistent sampler; if it is unavailable
        # (ATHENA_DIAG=0), fail open and poke — old behavior.
        if [ -n "$csv" ] && [ -r "$csv" ]; then
            util=$(tail -1 "$csv" 2>/dev/null | awk -F',' 'NF>=7{gsub(/ /,"",$7); printf "%d", $7+0}')
            if [ -n "$util" ] && [ "$util" -ge "${ATHENA_KEEPALIVE_MAX_UTIL:-15}" ]; then
                continue    # GPU busy → context warm → skip this poke
            fi
        fi
        # Deep probe (§20): /health is a hardcoded "ok" served by an independent HTTP
        # thread pool — a llama-server whose decode loop is wedged inside a CUDA call
        # answers /health forever while every /completion hangs, and the watchdog's
        # health poll is blind to it. This poke IS a real decode, so report it:
        # 3 consecutive failures WITH /health still green = the wedge signature →
        # flag the watchdog. A down/restarting server (health also failing) is the
        # watchdog fast path's job, not a wedge.
        if curl -sf -m 10 http://127.0.0.1:8080/completion \
             -d '{"prompt":" ","n_predict":1,"temperature":0,"cache_prompt":false}' >/dev/null 2>&1; then
            kfail=0; rm -f "$LLAMA_WEDGE" 2>/dev/null
        elif curl -sf --max-time 2 http://127.0.0.1:8080/health >/dev/null 2>&1; then
            kfail=$((kfail+1))
            if [ "$kfail" -ge 3 ]; then touch "$LLAMA_WEDGE" 2>/dev/null; kfail=0; fi
        else
            kfail=0
        fi
    done
}

# orpheus_watchdog (§20): supervise the TTS daemon the same way llama_watchdog
# supervises the backend — run 20260702-134528 proved a dead orpheus-speak freezes
# the brain (silent .done wait) with no recovery path. Detection is kill -0 only
# (the daemon has no HTTP surface); recovery is a byte-identical respawn. The
# respawned binary's own startup-recovery consumes a stale COMPLETED trigger and
# writes the .done that releases a blocked brain — without that binary fix a
# respawn alone would NOT unblock it (a completed trigger never fires inotify
# again). Rolling cap and stop sentinel shared with llama_watchdog.
orpheus_watchdog() {
    set +e; trap - ERR
    trap 'exit 0' TERM INT
    local WD_LOG="${RUN_DIR:-/dev/shm}/watchdog.log"
    _olog() { echo "[$(date '+%F %T.%3N')] orpheus-wd[$BASHPID]: $*" >> "$WD_LOG" 2>/dev/null; }
    _olog "armed — guarding orpheus-speak (interval ${WD_INTERVAL}s, cap ${WD_MAX_RESTARTS}/${WD_WINDOW}s)"
    local -a stamps=()
    local pid t cutoff
    while :; do
        [ -f "$LLAMA_WD_STOP" ] && { _olog "stop sentinel — exiting"; exit 0; }
        sleep "$WD_INTERVAL"
        [ -f "$LLAMA_WD_STOP" ] && { _olog "stop sentinel — exiting"; exit 0; }
        pid=$(cat "$ORPHEUS_PIDFILE" 2>/dev/null)
        [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && continue
        cutoff=$(( SECONDS - WD_WINDOW ))
        local -a kept=()
        for t in "${stamps[@]}"; do [ "$t" -ge "$cutoff" ] && kept+=("$t"); done
        stamps=("${kept[@]}")
        if [ "${#stamps[@]}" -ge "$WD_MAX_RESTARTS" ]; then
            _olog "GIVING UP: ${#stamps[@]} restarts within ${WD_WINDOW}s — TTS daemon hard-down (brain unaffected; TTS offline)"
            exit 0
        fi
        stamps+=("$SECONDS")
        _olog "orpheus-speak DOWN (pid=${pid:-none}); restart #${#stamps[@]}/${WD_MAX_RESTARTS}"
        [ -f "$LLAMA_WD_STOP" ] && { _olog "stop before respawn — exiting"; exit 0; }
        start_orpheus_speak
        _olog "respawned pid $PID_ORPHEUS — binary startup-recovery releases any waiting brain"
    done
}

start_llama_server

# Wait for an HTTP success from this live backend, within a wall-clock bound.
# A loading server returns HTTP 503; curl without -f called that "ready".
# Bound each request as well, so one stalled connection cannot defeat the wait.
wait_llama_ready() {
    local deadline=$((SECONDS + 60)) remaining probe_s
    echo "[launch-athena] waiting for llama-server to start..."
    while (( SECONDS < deadline )); do
        if [ "${ATHENA_STOP_STATUS:-0}" != 0 ]; then return "$ATHENA_STOP_STATUS"; fi
        if ! kill -0 "$PID_LLAMA" 2>/dev/null; then
            echo "[launch-athena] ERROR: llama-server exited unexpectedly"
            return 1
        fi
        remaining=$((deadline - SECONDS))
        # curl interprets a zero timeout as unbounded. The clock can cross
        # the deadline between the loop condition and the liveness check.
        if (( remaining <= 0 )); then break; fi
        probe_s=2
        if (( remaining < probe_s )); then probe_s=$remaining; fi
        if curl -sf --max-time "$probe_s" http://127.0.0.1:8080/health >/dev/null 2>&1; then
            echo "[launch-athena] llama-server ready."
            return 0
        fi
        sleep 0.5
    done
    echo "[launch-athena] ERROR: llama-server did not start within 60s"
    return 1
}
wait_llama_ready || exit "$?"

# Arm the watchdog + keepalive now that llama-server is healthy (this also covers the
# multi-minute talk-llama model load that follows). Backgrounded ⇒ never trips set -e
# or the ERR trap; the restart path uses its OWN bounded re-poll, never exit 1.
rm -f "$LLAMA_WD_STOP" 2>/dev/null
if [ "$ATHENA_WATCHDOG" != "0" ]; then
    llama_watchdog & PID_WATCHDOG=$!
    echo "[launch-athena] llama-server watchdog armed (pid $PID_WATCHDOG; ATHENA_WATCHDOG=0 to disable)"
fi
if [ "$ATHENA_KEEPALIVE" != "0" ]; then
    llama_keepalive & PID_KEEPALIVE=$!
    echo "[launch-athena] llama-server keepalive armed (pid $PID_KEEPALIVE; ATHENA_KEEPALIVE=0 to disable)"
fi

# ── Terminal 2: orpheus-speak daemon (SNAC decoder) ───────────────────────────

echo "[launch-athena] starting orpheus-speak daemon..."

# --diag (gated on ATHENA_DIAG) enables the real-time device-fill heartbeat and
# stall-run markers, so a developing underrun is visible live (r24.6 WO-53:
# they land in orpheus-speak.log now, not athena.log).
DIAG_FLAG=()
[ "$ATHENA_DIAG" != "0" ] && DIAG_FLAG=(--diag)

# --capture-dir (gated on ATHENA_TTS_CAPTURE) dumps per-session tokens/codes/wav/meta
# for garble diagnosis. Land it under the per-run diag dir when one exists, else a
# standalone dir, so captures sit next to gpu.csv / orpheus-server.log for the run.
CAPTURE_FLAG=()
if [ "$ATHENA_TTS_CAPTURE" != "0" ]; then
    TTS_CAPTURE_DIR="${RUN_DIR:-$DIAG_DIR}/tts-capture"
    if mkdir -p "$TTS_CAPTURE_DIR" 2>/dev/null; then
        CAPTURE_FLAG=(--capture-dir "$TTS_CAPTURE_DIR")
        echo "[launch-athena] TTS capture ON -> $TTS_CAPTURE_DIR (ATHENA_TTS_CAPTURE=0 to disable)"
    else
        echo "[launch-athena] WARNING: could not create $TTS_CAPTURE_DIR — TTS capture off"
    fi
fi

# start_orpheus_speak: launch the TTS daemon EXACTLY as configured and record its
# live PID in PID_ORPHEUS and $ORPHEUS_PIDFILE (the values cleanup() and
# orpheus_watchdog trust). exec keeps $! == the daemon's PID (§12 pattern).
# r24.6 WO-48 (S19 G4): --control wires the prosody/breath control file that
# talk-llama has been writing every turn ("$TRIGGER_FILE.ctl" — rate/pitch/
# pause/gain). orpheus-speak only reads it if --control is passed, which this
# launcher never did: zero "[orpheus-speak] prosody:" lines in 59 sessions, the
# between-chunks breath never fired once. The r21-B voice feature is now live —
# expect prosody: lines in orpheus-speak.log.
# r24.6 WO-53: the daemon's output gets its OWN timestamper into its own log
# (same shape as start_llama_server), instead of inheriting the session's merged
# pipe where its 989 glued records came from. Watchdog restarts re-enter here,
# so respawns keep the same log (append).
start_orpheus_speak() {
    ( ulimit -c "$CORE_ULIMIT"
      exec "$ORPHEUS_SPEAK" \
          --watch "$TRIGGER_FILE" \
          --control "$TRIGGER_FILE.ctl" \
          --snac "$SNAC_MODEL" \
          --play "aplay -q" \
          --stream-tts \
          "${DIAG_FLAG[@]}" \
          "${CAPTURE_FLAG[@]}" \
          -v --verbose --diag
    ) > >(_ts >> "$ORPHEUS_SPEAK_LOG") 2>&1 &
    PID_ORPHEUS=$!
    echo "$PID_ORPHEUS" > "$ORPHEUS_PIDFILE" 2>/dev/null
}

start_orpheus_speak

# Give it a moment to load the ONNX model
sleep 2

if ! kill -0 "$PID_ORPHEUS" 2>/dev/null; then
    echo "[launch-athena] ERROR: orpheus-speak exited unexpectedly"
    exit 1
fi

# Arm the orpheus-speak watchdog (§20): a TTS-daemon death no longer strands the
# brain — the respawned binary's startup-recovery releases any pending .done wait.
if [ "$ATHENA_WATCHDOG" != "0" ]; then
    orpheus_watchdog & PID_ORPHWD=$!
    echo "[launch-athena] orpheus-speak watchdog armed (pid $PID_ORPHWD; ATHENA_WATCHDOG=0 to disable)"
fi

# ── Terminal 3: talk-llama (voice assistant) ──────────────────────────────────

echo "[launch-athena] starting talk-llama..."

# Assemble --memory args only when MEMORY_DIR is set, so disabling is a one-line
# comment. An empty array splat expands to nothing (safe under set -u in bash 4.4+).
MEMORY_ARGS=()
if [ -n "${MEMORY_DIR:-}" ]; then
    mkdir -p "$MEMORY_DIR"
    # r24.6 (WO-65, decision DD5, resolves S19 I2): the demo flag
    # `--personality-reflect-every 1` is deleted — it lowered the personality-
    # revision threshold to 10 and let S19 rewrite her core self-description
    # three times in 2.5 hours. Mid-session revision itself stays enabled, at
    # the shipped default cadence.
    MEMORY_ARGS=(--memory "$MEMORY_DIR" --memory-words 2048 --time-refresh-min 15)
    echo "[launch-athena] memory enabled: $MEMORY_DIR"
fi
echo "[launch-athena] ═══════════════════════════════════════════════════"
echo "[launch-athena] Athena is ready. Speak into your microphone."
echo "[launch-athena] Press Ctrl+C to quit."
echo "[launch-athena] ═══════════════════════════════════════════════════"
echo ""

export SDL_AUDIODRIVER=pulse

# boost the Logi headset mic so barge-ins clear the 0.0020 trigger
# pactl set-source-volume "$(pactl list short sources | grep -i logi | grep -iv monitor | head -1 | cut -f2)" 250%

# ── Qwen3.5-397B-A17B UD-Q3_K_XL settings (researched + MEASURED 2026-06) ──
#
# Sampling ACTUALLY PASSED below (comment reconciled r24.6, WO-66 — the old
# text described the official preset as if it were what ran; it was not):
#   temp=0.70, top_p=0.80, top_k=20, min_p=0.0,
#   presence_penalty=0.5, repeat_penalty=1.0 (disabled), repeat_last_n=512
# — a deliberate, field-run departure from the OFFICIAL Unsloth preset for
# Instruct (non-thinking) mode, "Reasoning tasks" column
# (unsloth.ai/docs/models/qwen3.5):
#   temp=1.0, top_p=0.95, top_k=20, min_p=0.0,
#   presence_penalty=1.5, repeat_penalty=1.0 (disabled)
#
# DRY (r24.12, §E.3 — documented here because nothing on the command line says
# so): the r14 DRY chain is IN FORCE BY DEFAULT and is NOT passed below. The
# defaults live in talk-llama.cpp's params — --dry-multiplier 0.75,
# --dry-base 1.75, --dry-allowed-length 2, --dry-penalty-last-n 4096 — and they
# are what stop the stock-phrase loops ("But here's the thing:", the double-
# question closer). Pass --dry-multiplier 0 to disable the sampler entirely and
# restore the pre-r14 chain byte-for-byte; do NOT lower it as a fix for
# anything else (plan §E.6 rejects that explicitly).
# This chain is also what WO-I7's spacing guard BELTS: DRY penalises a token by
# its text, so the leading-space twin of a penalised piece could be sampled in
# its place and her speech lost the space between two words (S22 §C.2.2). The
# guard brackets the DRY sampler in the main chain (dg_pre_init / dg_post_init
# beside aseam::dry_space_twin) and is on by default; ATHENA_DRY_SPACE_GUARD=0
# restores r24.11 — the guard off, DRY unchanged.
# kept here for reference / A-B. The tighter temp/top-p and the low presence
# penalty are what S19 and the r24.x field sessions actually validated for
# spoken style; restore the preset values only as a measured experiment.
# Instruct mode applies because talk-llama feeds a raw transcript (no chat
# template -> no <think> blocks; a think block would be spoken aloud anyway).
#
# KV CACHE: bf16. q8_0 was tried first (would allow full 262144 ctx) and
# produced degraded output in the field — including a literal <think> token
# emitted into the transcript by turn 3 — matching Unsloth's documented
# quantized-KV symptom for this family ("if gibberish, try bf16 KV").
#
# CONTEXT: --ctx-size 80000 BY DESIGN (r24.6 decision DD3, raised to 80000 and
# re-affirmed as r24.12 decision 10; comment reconciled by WO-66, resolves S19
# I1 — this block used to justify 131072 while the invocation passed a smaller
# number, inviting the next reader to "fix" it. Do not.)
# r24.12 (review): this paragraph still said 40000 while the invocation below
# passes 80000 — the same drift WO-66 was written to end, one number over. The
# figure is 80000 in both places now; the reasoning is unchanged and is about
# the SHAPE of the window, not its size.
# 80000 is NOT a resource limit: it is chosen so the compaction / memory-roll
# machinery (notice -> arm -> force -> cut) runs on the hot path of every
# session of real length, which is what the whole r24.6 Tier-1 fix set is
# sized and tested against. Raising it would not crash — it would silently
# move compaction out of every ordinary session and un-test it. (A 2,040-token
# acuity look is ~2.6% of this window; the r24.12 vision work orders were sized
# against it — see the camera block.)
# For the record, 131072 DOES fit; measured ledger of the 2026-06-10 load
# (19,186 MiB free at Qwen load with Orpheus q8_0 + Whisper + SNAC resident):
#   dense weights (CUDA0 model buffer)   9,129.7 MiB   measured
#   KV bf16 @131072 (15 full-attn lyrs)  3,840.0 MiB   = 15 x 131072 x 1024 x 2 B
#   GDN recurrent state                    186.3 MiB   measured
#   compute buffer (worst observed)      2,498.7 MiB   measured (reserve 1832 grew +666)
#   total ~15,656 MiB -> ~3.5 GiB headroom
# bf16 @262144 totals 19,496 MiB vs 19,186 free -> over by ~310 MiB: does NOT
# fit. @196608 fits with only ~1.6 GiB margin — too thin given the compute
# buffer's observed growth. 131072 remains the robust ceiling IF a large-
# context build is ever wanted — that would be a deliberate design change
# (re-read the R246 plan first), not a correction.
#
# --no-mmap: 165 GB split GGUF; read() into anonymous memory + --mlock avoids
# double residency in page cache. Experts measured: 161,318.6 MiB CUDA_Host.
#
# Speed: routed reads ~3.07 GB/token (10x60 experts @ ~3.25 bpw) — half the
# 122B-Q6's 6.2 GB/token (measured 7-10 tok/s). Expect ~9-14 tok/s.
# Adopted after field A/B (2026-06-12): 0-15 / -t 16 measured ~+30% decode
# vs 0-7 / -t 8 on the bandwidth-bound MoE expert reads (4.8 -> ~6.4+ tok/s,
# near the DDR5-4000 ceiling). Cores 16-23 stay free to absorb audio/daemon
# wakeups so llama.cpp's per-op barrier never waits on a preempted thread.
# -t 24 expected flat-to-negative; re-measure via chunk-gap pacing if tried.
#
# BARGE-IN: speak over Athena and she stops within ~0.5 s, listens, then
# either pivots (real interruption -> LLM state rollback, only the words she
# got out stay in the transcript, closed with an em-dash) or resumes the
# sentence (false alarm: cough, door, nothing transcribed). Knobs:
# Values below are headset-tuned from the 2026-06-12 18:16 field session
# (5 missed barges analyzed): soft first-attempt speech measured
# 0.0017-0.0038 RMS, earcup BLEED holds the EMA floor at 0.0005-0.0008
# while she speaks, and silence floor is ~0.0002.
#   --barge-rms 0.0020       absolute MINIMUM threshold (high-passed 300 ms
#                            RMS). 2026-06-13 demo: your intentional barges
#                            ran 0.0024-0.0059, but soft BACKCHANNELS ("sure",
#                            "absolutely") landed 0.0019-0.0023 and cut her
#                            off. 0.0020 gates the softest of those at the
#                            energy stage; the rest are caught by content
#                            (is_backchannel) which resumes her instead of
#                            derailing. Miss a soft deliberate barge -> 0.0015
#                            (the filter still prevents backchannel derail);
#                            still cut off mid-thought -> 0.0024.
#   --barge-ratio 1.5        a barge must also exceed this multiple of the
#                            self-measured playback-time ambient. On a
#                            headset that ambient IS earcup bleed: at the
#                            old 4.0 the bleed-fed floor (0.0008) raised the
#                            bar to 0.0032 and beat the absolute — misses.
#                            1.5 keeps the absolute in charge (1.5 x 0.0008
#                            = 0.0012 < 0.0015) while still scaling if room
#                            noise genuinely jumps. OPEN SPEAKERS: go back
#                            to 4.0+ — there the ratio arm is the defense.
#   --barge-ms 150           sustained energy required (consecutive polls;
#                            one cold poll resets the run). Two misses were
#                            hot at 0.0035-0.0038 but never held 300 ms.
#                            Each poll already averages a 300 ms window, so
#                            150 latches one-word interjections. Breath/
#                            plosive false triggers -> raise to 200 first.
#   --barge-blackout-ms 200  arm delay after the first sentence flush. Her
#                            voice cannot reach a headset mic, so the old
#                            700 ms (first-audio + AEC settle) is dead time;
#                            calibration on room ambience is valid anytime.
#                            OPEN SPEAKERS: restore 700.
# With -pe set, a once-per-second "barge monitor: rms/peak/floor/threshold"
# line shows your live levels — calibrate from those numbers, not by feel
# (r24.6 WO-53: stderr, so read them in athena-diag.log, e.g. tail -f).
# A false trigger costs a ~1-2 s hiccup (validation -> false-alarm resume);
# a missed barge costs the feature. Tune aggressive.
# Cost when idle: one state snapshot per turn (~0.5 GB host RAM reused,
# ~30-90 ms typical; grows ~30 KB/token of context + 186 MB GDN state, so
# ~4 GB / a few hundred ms at the full 131K window).
#
# ── prosodic endpointing (--endpoint) ────────────────────────────────────────
# Replaces the single fixed end-of-turn wait (--vad-last-ms, 400 ms here) with
# two, chosen from the pitch/energy contour of the speech just before the pause:
#   --endpoint-short-ms 800   turn-final FALL/trail-off — the FLOOR: no path can
#                             end a turn on less than 800 ms of silence (§23.7;
#                             was 350, which cut mid-thought pauses)
#   --endpoint-long-ms 1100   a flat/rising "not done yet" pause -> keep
#                             listening. r24.6 (WO-51, S19 G6): was 800 == short,
#                             which made the turn-final classification a DEAD
#                             KNOB — both branches returned 800, and S19's one
#                             endpointer failure was exactly here: an 832 ms
#                             mid-sentence pause after "…I did not make any
#                             edits in your memory." got "-> continue (800 ms)"
#                             and was cut at 832 ms anyway. 1100 gives the
#                             "not done yet" verdict real headroom; a wrong long
#                             call costs at most 300 ms of extra wait.
# This is the right shape for an already-snappy 400 ms setup: the win is the long
# branch (stop cutting off mid-thought pauses), with a slightly faster turn-final.
# --vad-last-ms stays the fallback when --endpoint is absent. A wrong "final" call
# is caught by the barge-in path, so the downside is bounded by the old behavior.
# Decision thresholds (tune from the -pe log, which now prints one
# "endpoint f0_slope=.. e_slope=.. -> turn-final/continue" line per turn —
# in athena-diag.log as of r24.6 WO-53):
#   --endpoint-f0-fall 60       Hz/s of F0 fall that counts as falling
#   --endpoint-energy-decay 4.0 log-energy slope (/s) that counts as trailing off
# Read f0_slope/e_slope on turns you KNOW were final vs mid-thought, then set the
# thresholds where they separate cleanly. Loosen (lower) to catch more finals;
# tighten (raise) if it answers over your pauses.
#
# ── Silero streaming VAD (--vad-engine silero, the default) ──────────────────
# The neural Silero VAD streams per-frame speech probabilities and ends the turn
# on sustained REAL silence — fixing the 10-30 s hangovers the energy vad_simple
# hit once speech scrolled out of its relative window. --endpoint still applies:
# prosody picks the silence TARGET per turn (--endpoint-short/long below) and
# Silero detects the silence. If the model is missing it falls back to the energy
# path (--vad-last-ms 400 + the absolute-floor backstop). Silero knobs (defaults):
#   --silero-threshold 0.5      speech-probability cutoff
#   --silero-min-run-ms 100     HYSTERESIS: consecutive speech (ms) needed to count
#                               as speech. Single-frame Silero spikes (noise/breath)
#                               are shorter and so can't reset the silence clock —
#                               this is the fix for "turn never ended" flapping.
#   --silero-silence-ms 700     turn-end silence used only when --endpoint is OFF
#   --silero-min-speech-ms 120  ignore sub-utterance blips (cumulative)
#   --silero-poll-ms 100        capture + inference cadence
#   --silero-debug              per-poll [silero-dbg] trace (prob + accumulators)
#                               — NOT passed as of r24.6 (WO-53), see below
# NOTE: THE 800 ms FLOOR (CHANGES.MD §23.7, kept in r24.6). The prosodic
# turn-final decision (talk-llama.cpp:1768) is an OR of f0-fall/energy-decay and
# there is NO minimum-silence floor on the Silero path, so with the original
# 350/1200 split a sincere falling clause + a >350 ms dramatic pause fired the
# SHORT (350 ms) target and cut beat-4's final sentence (run 20260704-222137,
# silence=352ms). Raising SHORT to 800 fixed that structurally: every turn needs
# at least 800 ms of trailing silence whatever prosody says. 800 = ~30-95 ms
# above that run's genuine turn-ends (704-768 ms) and above the ~640 ms within-
# turn pause median (research), so dramatic pauses up to 800 ms are safe.
# r24.6 (WO-51) re-opens the gap UPWARD only: long-ms 800 -> 1100, so a
# flat/rising "not done yet" contour now buys real extra patience (S19's 832 ms
# split — see the endpoint comment above). The premature-fire class stays
# eliminated because the MINIMUM is still short-ms == 800; do NOT lower
# short-ms below long-ms's old failure territory without adding a min-silence
# floor in silero-turn-state.h. Snappier turn-taking: drop SHORT to 700 (proven
# 23/23 clean, no margin). --vad-last-ms is a red herring here (Silero path
# ignores it).
# --silero-debug was deleted in r24.6 (WO-53; the launcher's own note here used
# to say "delete that line once dialled in"): [silero-dbg] was 61,143 of S19's
# 72,335 log lines — 84.5 % of the log — and 95.9 % of those were the byte-
# identical idle line "wait maxp=0.00 speech=0 sil=0 run=0". Costs nothing
# diagnostic; re-add the flag below for a tuning session.
#
# PROSODY-TO-EXTEND (CHANGES.MD §23.8): the robust use of prosody — grant MORE time
# when the acoustics say "not done", NEVER less (the §23.7 800 ms floor still holds;
# extend only raises the target via max()). --endpoint-extend-ms 1200: on a "not-done"
# contour, wait 1200 ms instead of 800. "Continuing" = ProsodicEndpointer.continuing:
# !turn_final AND (F0 slope > +f0-rise, voiced) AND (log-energy slope > +energy-rise,
# voiced) — BOTH a pitch rise AND rising/held energy required (an AND, not OR).
#   WHY AND (review C1/C2): a wrong EXTEND is UNRECOVERABLE — Athena answers LATE while
#   the user, done, stays silent, so no barge-in recovers it (a wrong SHORT is recovered
#   by a barge). A genuinely turn-final yes/no QUESTION rises in pitch but its energy
#   TAPERS into the pause -> energy-rise fails -> NOT extended. A real held continuation
#   ("...and,") both rises and sustains energy -> extended. extend capped at base+400 to
#   bound the dead-air of any residual false-positive.
#   DIALS: still over-extending questions -> raise --endpoint-f0-rise or set
#   --endpoint-extend-ms 0 (off, reverts to flat 800). Want more coverage (accept the
#   question risk) -> lower --endpoint-f0-rise / switch continuing to || in the source.
#   --endpoint-f0-fall/-energy-decay select short (800) vs long (1100) — live
#   again as of r24.6 WO-51 (they were inert while short==long==800); extend is
#   layered on top via max(). Watch the [silero] "-> not-done/extend
#   (1200 ms)" debug lines: they should fire on held mid-thought rises, NOT genuine ends.
# NEVER core-dump the brain: its ~161 GiB mlock'd RSS would write a catastrophic core
# even though core_pattern is set for the small clients (CHANGES.MD §12).
#
# r24.6 WO-53 (S19 G8): talk-llama's STDERR gets its own timestamper into
# athena-diag.log. stdout — her speech, streamed token-by-token with no
# newline until the sentence ends — is deliberately left EXACTLY as it was, on
# this script's own stdout, so the caller's `| ts | tee athena.log` (the
# .desktop wrapper) and the on-screen incremental display are untouched. Do not
# "clean this up" by merging the two streams again (2,028 glued log records),
# and do not newline-terminate the token stream in code instead (that fix
# measured 1,080 display lines instead of 40 — it chops her speech onto a new
# line per token).
ulimit -c 0
# ── r24.17 tracked brain launch ────────────────────────────────────────────
# A stop during backend startup was latched before a brain PID existed. Never
# create a new, unsignalled brain after that request; cleanup owns the clients
# already started. The PID-assignment handoff below remains separately guarded.
if [ "${ATHENA_STOP_STATUS:-0}" != 0 ]; then exit "$ATHENA_STOP_STATUS"; fi
ATHENA_BRAIN_ARGS=(taskset -c 0-15 "$TALK_LLAMA" \
    -ml "$QWEN_MODEL" \
    -mw "$WHISPER_MODEL" \
    --ctx-size 80000 \
    --mlock \
    --no-mmap \
    --cpu-moe \
    -t 16 -ngl 99 \
    -ctk bf16 -ctv bf16 -fa \
    --temp 0.70 --top-p 0.80 --top-k 20 --min-p 0.00 \
    --presence-penalty 0.5 --repeat-penalty 1.0 --repeat-last-n 512\
    --reasoning off \
    -s "$SPEAK_DAEMON" -sf "$SPEAK_FILE" \
    --stream-file "$TRIGGER_FILE" \
    --barge-in \
    --barge-rms 0.0020 \
    --barge-ratio 1.5 \
    --barge-ms 150 \
    --barge-blackout-ms 200 \
    "${MEMORY_ARGS[@]}" \
    -p Igor -bn Athena \
    -mt 256 -vms 25000 \
    --vad-engine silero \
    --vad-model "$VAD_MODEL" \
    --vad-last-ms 400 \
    --vad-window-ms 700 \
    --endpoint \
    --endpoint-short-ms 800 \
    --endpoint-long-ms 1100 \
    --endpoint-f0-fall 60 \
    --endpoint-energy-decay 4.0 \
    --endpoint-extend-ms 1200 \
    --endpoint-f0-rise 50 \
    --endpoint-energy-rise 0.5 \
    --silero-min-run-ms 100 \
    --mmproj "$ATHENA_DIR/models/mmproj-BF16.gguf" \
    --camera-dev /dev/video0 \
    --camera-res 1920x1080 \
    --vision-work-edge 800 \
    --vision-acuity-edge 1920 \
    --vision-acuity-mode scale \
    --image-min-tokens 2040 \
    --look-max 15 \
    --recall-max 6 \
    -pe)
if [ "$ATHENA_LAUNCH_SUPERVISE" != "0" ]; then
    # Noninteractive job control is off, so the asynchronous child is not already
    # a group leader: setsid execs in place and $! is the actual brain PID.
    set +m
    # r24.20 review (F1 a, measured): under the documented `| ts | tee athena.log`
    # forms the terminal's Ctrl+C kills ts and tee, and this launcher's stdout
    # is a pipe with no reader from that moment. Its cleanup then died of
    # SIGPIPE at an `echo` — before, or in the middle of, the ordered stops,
    # leaving MPS up and the clocks locked (and in r24.20, a brain never
    # signalled). From here on a failed write is EPIPE, not death. The brain
    # inherits it: a speech token printed into that dead pipe in the ~200 ms
    # before it polls its stop flag can no longer kill it either. It writes to
    # no other pipe (its capture commands write files; TTS goes by file), so
    # nothing else changes for it.
    trap '' PIPE
    exec {ATHENA_TALK_LOG_FD}> >(_ts >> "$ATHENA_DIAG_LOG")
    PID_TALK_LOG=$!
    ATHENA_TALK_STARTING=1
    # Only the directly owned brain receives this internal coordinator field.
    # setsid execs in place here; the trap owner remains its actual parent.
    # Expand BASHPID before the asynchronous command forks; expanding it in
    # that command's environment assignment would advertise the child's PID.
    ATHENA_TALK_PARENT_PID=$BASHPID
    ATHENA_SHUTDOWN_COORDINATOR_PID="$ATHENA_TALK_PARENT_PID" setsid "${ATHENA_BRAIN_ARGS[@]}" 0<&0 2>&"$ATHENA_TALK_LOG_FD" &
    PID_TALK=$!
    PGID_TALK=$PID_TALK
    exec {ATHENA_TALK_LOG_FD}>&-
    ATHENA_TALK_STARTING=0
    if [ "$ATHENA_PENDING_SIGNAL" != 0 ]; then exit "$ATHENA_PENDING_SIGNAL"; fi
    # r24.20 review (F1): `wait` returns >128 as soon as a trapped signal
    # arrives, with the brain unreaped; the handler has forwarded the stop and
    # left its status here. Exit with it — from the main line, never from the
    # trap — and cleanup waits for the brain's pass. A return with no stop
    # recorded is the brain's own exit (normal or by signal): reaped.
    while :; do
        if wait "$PID_TALK"; then ATHENA_TALK_STATUS=0; else ATHENA_TALK_STATUS=$?; fi
        if [ "$ATHENA_STOP_STATUS" != 0 ]; then exit "$ATHENA_STOP_STATUS"; fi
        _athena_brain_probe "$PID_TALK"
        if [ "$ATHENA_TALK_STATUS" -le 128 ] || [ -z "$ATHENA_BRAIN_STATE" ]; then break; fi
        # >128 with the brain still alive and no stop recorded: a trapped
        # signal whose handler chose not to stop (none today); keep waiting.
    done
    TALK_REAPED=1
    # Preserve the brain's ordinary exit status; a signal trap exits with its
    # own 130/143 instead. EXIT cleanup performs exactly one ordered teardown.
    exit "$ATHENA_TALK_STATUS"
else
    ATHENA_SHUTDOWN_COORDINATOR_PID= "${ATHENA_BRAIN_ARGS[@]}" 2> >(_ts >> "$ATHENA_DIAG_LOG")
fi
# ── end r24.17 tracked brain launch ─────────────────────────────────────────


# r24.12 (WO-V1): --camera-res 1920x1080 — DD22 (decision 2026-09-02):
# DD18 withdrawn — the camera does 1080p in MJPG; the S19/S20 fallback was
# ffmpeg's raw-format preference, invisible at -loglevel error; capture returns
# to 1920x1080 and the encode cost is bounded by the working edge, not the
# capture.
#   There is no V4L2 negotiation in ATHENA at all: CameraPort shells out to
#   ffmpeg, and ffmpeg's v4l2 demuxer tries every RAW pixel format (YUYV among
#   them) before MJPEG, accepting the driver's silent size substitution — the
#   camera's YUYV modes top out at 640x480, its 1080p is MJPG only, and the
#   "The V4L2 driver changed the video from 1920x1080 to 640x480" line is
#   logged at INFO, which -loglevel error hides. Every S22 still was a 640x480
#   baseline JPEG for that reason and no other. r24.12 asks for MJPEG first
#   (`-input_format mjpeg`, accepted only if the file's own SOF header reads),
#   then the r24.11 command, then fswebcam, and logs which rung delivered:
#   `[vision] capture via mjpeg: 1920x1080 (requested 1920x1080)` is the S23
#   grep, `capture fell back` must be absent. The WO-35 fallback detection
#   STAYS ARMED (inspect_capture compares the SOF dims against the request).
#   ATHENA_CAPTURE_MJPEG=0 restores the r24.11 command byte for byte;
#   ATHENA_CAPTURE_COPY=1 (the one opt-in) writes the camera's native JPEG
#   (-c:v copy -bsf:v mjpeg2jpeg) instead of ffmpeg's re-encode.
#
# r24.12 (WO-V2): --vision-work-edge 800 — the WORKING COPY. The full 1080p
#   frame is what she holds and what the album keeps; what mtmd is handed is a
#   copy resampled to a long edge of 800: 1920x1080 -> 800x450, which mtmd's
#   smart_resize rounds to 800x448 = 25x14 = 350 image tokens ≈ 22 s per look
#   (encode ≈6 s + image prefill ≈16 s, priced from the S22 measurements:
#   300 tokens = 5.0 s + 14.2 s, 918 tokens = 18.7 s + 33 s). Native 1080p is
#   2040 tokens ≈ 115 s and would DEFER every look past the turn that asked.
#   0 = native = r24.11. ATHENA_VISION_WORK_EDGE overrides the flag. At edge
#   800 an ordinary look prices at ≈22 s, so the announce threshold
#   ATHENA_VISION_ANNOUNCE_S ships at 25 s (the plan's 20 was written for the
#   640 edge): an ordinary look keeps "Let me take a look."; the edge-800
#   portrait recalls (≈29 s), native portraits (≈52 s) and acuity looks
#   (≈115 s) announce the wait. Lower it to 20 to hear the long line on every
#   look; 0 = the r24.11 lines always.
#
# r24.12 (WO-V2): --vision-acuity-edge 1920 --vision-acuity-mode scale — the
#   ACUITY look. When his words ask her to READ ("can you read any of it",
#   "word for word", "what does it say" — never "read me that list", which is
#   memory), the look is encoded at the full frame: 1920x1080 -> 1920x1088 =
#   60x34 = 2040 tokens ≈ 115 s, and the drain waits ATHENA_ACUITY_WAIT_MS
#   (120000 ms) for it instead of the 1-s ordinary wait, announcing "I'm
#   reading it — one moment." — so the reading reply is generated with the
#   picture in front of her. `crop` would instead take a centred 1920x1080
#   window at native pixels (a no-op at this edge; meaningful below it). With
#   a held frame under two minutes old, a licence and no new invitation, an
#   acuity request commits a re-look at the acuity size ("let me look
#   closer"). ATHENA_ACUITY_LOOK=0 makes every look a working-edge look.
#
# r24.12 (WO-V2): --image-min-tokens 2040 — the acuity token count. The
#   capture-size hedge (WO-84) fires ONLY on a look that was asked to read
#   (acuity_requested) and was served below this many tokens — never on an
#   ordinary 350-token look. ATHENA_ACUITY_LOOK=0 restores the r24.11 test,
#   which at this value would hedge every working-edge look.
#
# Budgets: --look-max 15, --recall-max 6 (S1's real demand was four images;
# WO-V6 removes the class that spent a slot unprompted, WO-V12 stops charging
# a re-open of a picture still in her context). ATHENA_LOOK_MAX /
# ATHENA_RECALL_MAX env overrides also exist and take precedence over the flags.

# If talk-llama exits on its own, cleanup runs via the trap
