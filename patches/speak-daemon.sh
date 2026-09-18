#!/usr/bin/env bash
# speak-daemon.sh — talk-llama --speak wrapper for orpheus-speak --watch mode
#
# LEGACY/FALLBACK PATH. launch-athena-397b.sh runs talk-llama with --stream-file,
# so talk-llama streams sentences to the trigger file itself and this -s handler
# never runs. It is kept only as the non-streaming batch fallback. The path
# variables below stay in sync with launch-athena-397b.sh (SPEAK_FILE /
# TRIGGER_FILE / DONE_FILE) because both DERIVE the same ATHENA_DIR from their own
# location — talk-llama's -sf writes the speak file, this script reads it, and
# orpheus-speak watches the trigger, all under the same repo dir.
#
# Manual setup (three terminals), reflecting the current ATHENA/ layout:
#
#   Terminal 1 (llama-server — Orpheus TTS backend, start first, leave running):
#     $ATHENA_DIR/llama.cpp/build/bin/llama-server -m $ATHENA_DIR/models/orpheus-3b-0.1-ft-UD-Q4_K_XL.gguf -c 13824 -np 4 -ngl 99 --host 127.0.0.1 --port 8080 --cache-type-k f16 --cache-type-v f16 -fa on
#
#   Terminal 2 (daemon — start second, leave running):
#     $ATHENA_DIR/orpheus/build/orpheus-speak --watch $ATHENA_DIR/speak_tts.txt --snac $ATHENA_DIR/orpheus/snac24_dynamic_fp16.onnx --play "aplay -q" -v
#
#   Terminal 3 (talk-llama — STT + Qwen3.5-397B):
#     $ATHENA_DIR/whisper.cpp/build/bin/whisper-talk-llama -ml $ATHENA_DIR/models/Qwen3.5-397B-A17B-UD-Q3_K_XL-00001-of-00005.gguf -mw $ATHENA_DIR/models/ggml-small.en.bin --cpu-moe --temp 0.7 --top-p 0.8 --top-k 20 --min-p 0.00 -t 16 -ngl 99 -s $ATHENA_DIR/speak-daemon.sh -sf $ATHENA_DIR/speakfile.temp -p Igor -bn Athena -mt 128 -vms 15000 --presence-penalty 1.5 --repeat-penalty 1.0 -ctk bf16 -ctv bf16 -fa; rm $ATHENA_DIR/speakfile.temp
#
# MUCH faster than speak.sh because the ONNX session stays warm.

# Repo dir, resolved through symlinks — the same ATHENA_DIR the launcher derives
# (and exports), so these speak_tts.* paths match what orpheus-speak watches.
ATHENA_DIR="${ATHENA_DIR:-$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)}"
SPEAK_FILE="$ATHENA_DIR/speakfile.temp"
TRIGGER_FILE="$ATHENA_DIR/speak_tts.txt"
DONE_FILE="$ATHENA_DIR/speak_tts.done"

# r24.20: a consumed wrapper receipt otherwise leaves no local owner sequence.
# Reuse the existing daemon binary's descriptor-safe bounded writer. Checking a
# shell path before >> still lets a substituted FIFO block delivery or a symlink
# redirect the trace. The helper exits before loading models or starting curl.
# This runs at two batch boundaries, never at a microphone/PCM poll. A missing
# or mismatched installation leaves explicit incomplete evidence and continues.
tts_trace() {
    [[ "${ATHENA_TTS_PROTOCOL_TRACE:-1}" != 0* && -n "${ATHENA_TTS_PROTOCOL_TRACE_DIR:-}" ]] || return 0
    if ! "$ATHENA_DIR/orpheus/build/orpheus-speak" --trace-wrapper "$1" "$SESSION_ID" "$2"; then
        echo '[athena-trace] incomplete file=tts-wrapper.jsonl reason=helper' >&2
    fi
    return 0
}

if [ ! -f "$SPEAK_FILE" ]; then
    if [[ "${ATHENA_TTS_DELIVERY_PROOF:-1}" != 0* ]]; then exit 1; fi
    exit 0
fi

# r24.17: a failed sanitize/move used to enter the thirty-minute wait, and
# /tmp on another filesystem turns mv into copy/unlink (the daemon then sees
# CLOSE_WRITE, mistakes a batch for a stream and waits for a missing END).
# Stage beside the trigger and check every write. =0 restores the old path.
WRITE_COMMIT=1
if [[ "${ATHENA_TTS_WRITE_COMMIT:-1}" == 0* ]]; then WRITE_COMMIT=0; fi

# r24.19: the same per-session identity as the streaming brain. An earlier
# daemon playback must not satisfy this batch's wait or delete this trigger.
# The daemon excludes the first protocol line from synthesized text.
SESSION_ID=""
if [[ "${ATHENA_TTS_SESSION_ID:-1}" != 0* ]]; then
    SESSION_ID="${BASHPID:-$$}-${EPOCHREALTIME//./}-${RANDOM}-${RANDOM}"
fi

# Remove any stale done flag — LEGACY (unlabelled) sessions only. An
# identified session never unlinks a receipt it did not consume: the wait
# below skips a foreign receipt and the daemon replaces it atomically, which
# is the rule athena_tts_wire.h (read_report) and this file's own wait loop
# state. Unlinking here raced that replacement (r24.20 review, SPEECH F8).
# The legacy wait accepts ANY receipt, so a stale one must still go there.
if [[ -z "$SESSION_ID" ]]; then
    if ! rm -f "$DONE_FILE" && (( WRITE_COMMIT )); then exit 1; fi
fi

# Sanitize and write to trigger file (atomic via temp + mv)
if (( WRITE_COMMIT )); then
    TMPF=$(mktemp "$ATHENA_DIR/.speak_tts.XXXXXXXX") || exit 1
    trap 'rm -f -- "$TMPF"' EXIT
else
    TMPF=$(mktemp)
fi
sed -E \
    -e 's/\*\*([^*]*)\*\*/\1/g' \
    -e 's/\*([^*]*)\*/\1/g' \
    -e 's/`[^`]*`//g' \
    -e 's/^#{1,6} //g' \
    -e 's/https?:\/\/[^ ]*//g' \
    -e 's/\[([^]]*)\]\([^)]*\)/\1/g' \
    -e 's/<\|[a-z_]*\|>//g' \
    "$SPEAK_FILE" > "$TMPF"
status=$?
if (( WRITE_COMMIT && status != 0 )); then exit 1; fi

# Sanitizing a markup-only line may leave a newline, which has bytes but no
# speech. Reject that no-delivery case too; do not start an empty daemon session.
if [[ "${ATHENA_TTS_DELIVERY_PROOF:-1}" != 0* ]]; then
    filtered=$(<"$TMPF")
    if [[ -z "${filtered//[[:space:]]/}" ]]; then
        rm -f "$TMPF"
        exit 1
    fi
fi

if [ ! -s "$TMPF" ]; then
    rm -f "$TMPF"
    if [[ "${ATHENA_TTS_DELIVERY_PROOF:-1}" != 0* ]]; then exit 1; fi
    exit 0
fi

if [[ -n "$SESSION_ID" ]]; then
    TMPH=$(mktemp "$ATHENA_DIR/.speak_tts.XXXXXXXX") || exit 1
    trap 'rm -f -- "$TMPF" "$TMPH"' EXIT
    if ! { printf '%s\n' "---ATHENA_SESSION $SESSION_ID B---"; cat -- "$TMPF"; } > "$TMPH"; then exit 1; fi
    if ! mv -f -- "$TMPH" "$TMPF"; then exit 1; fi
fi

MOVE_ARGS=(-f)
# The trigger is one file, never a destination directory. GNU mv's -T also
# keeps a directory substitution from turning publication into a hidden move.
if (( WRITE_COMMIT )); then MOVE_ARGS+=(-T); fi
if mv "${MOVE_ARGS[@]}" -- "$TMPF" "$TRIGGER_FILE"; then
    # mv can succeed by placing the source INSIDE a misconfigured directory.
    # That actual wrapper probe never published the trigger the daemon reads.
    # Its trace must retain the failed boundary, even under the old wait path.
    if [[ -f "$TRIGGER_FILE" ]]; then tts_trace owner_publish B
    else
        tts_trace owner_publish_failed B
        if (( WRITE_COMMIT )); then exit 1; fi
    fi
else
    tts_trace owner_publish_failed B
    if (( WRITE_COMMIT )); then exit 1; fi
fi

# Wait for daemon to finish (it creates .done after playback)
# Timeout after 1800s (30 min) to avoid hanging forever (long stories need time)
for i in $(seq 1 36000); do
    if [ -f "$DONE_FILE" ]; then
        # r24.16: the wrapper discarded FAILED/empty reports and returned
        # success, so the non-streaming brain remembered speech that failed.
        # Only COMPLETE is affirmative delivery evidence. The opaque command
        # API can carry failure back through its existing exit status.
        # ATHENA_TTS_DELIVERY_PROOF=0 restores the old existence-only result.
        receipt=$(<"$DONE_FILE") # whole receipt: COMPLETE plus garbage is not proof
        if [[ -n "$SESSION_ID" ]]; then
            expected="SESSION $SESSION_ID"$'\n'
            # Leave a foreign report in place; unlinking it can race its newer
            # atomic replacement. The daemon will publish our own receipt.
            if [[ "$receipt" != "$expected"* ]]; then sleep 0.05; continue; fi
            receipt="${receipt#"$expected"}"
        fi
        trace_detail=INVALID
        if [[ "$receipt" == COMPLETE || "$receipt" =~ ^(FAILED|INTERRUPTED)\ [0-9]+\ [0-9]+$ ]]; then
            if (( ${#receipt} <= 80 )); then trace_detail="$receipt"; fi
        fi
        tts_trace receipt_consume "$trace_detail"
        if [[ "${ATHENA_TTS_DELIVERY_PROOF:-1}" != 0* ]]; then
            rm -f "$DONE_FILE"
            if [[ "$receipt" == "COMPLETE" ]]; then
                exit 0
            fi
            echo "[speak-daemon.sh] WARNING: speech delivery was not confirmed: $receipt" >&2
            exit 1
        fi
        rm -f "$DONE_FILE"
        exit 0
    fi
    sleep 0.05
done

echo "[speak-daemon.sh] WARNING: daemon did not signal completion" >&2
# r24.16: a timeout is likewise not completion; leave the old exit status at OFF.
if [[ "${ATHENA_TTS_DELIVERY_PROOF:-1}" != 0* ]]; then
    exit 1
fi
