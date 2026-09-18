#!/usr/bin/env bash
# athena-vision-check.sh — r20 P1 preflight for Athena's eyes.
# Run this ONCE on the rig before the first sighted session. It checks the
# camera path the live look uses (same commands, same defaults) and leaves a
# test frame you can feed to --vision-bench.
#
#   ./athena-vision-check.sh [device] [WxH]     defaults: /dev/video0 1920x1080
# r24.16: this diagnostic lagged behind CameraPort::start_capture: it tried
# raw-format auto negotiation first, so the MJPG-only 1080p camera could report
# a 640x480 "success". The fixture reproduces that ordering and invalid JPEGs.
# The default path follows the live MJPEG -> auto -> fswebcam ladder, validates
# each candidate, bounds each command, and keeps its own output directory.
# ATHENA_VISION_CHECK_LADDER=0 reaches the original diagnostic below unchanged.
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    echo 'Usage: athena-vision-check.sh [device] [WxH]'
    echo 'Captures one image when run; --help does not access the camera.'
    echo 'ATHENA_VISION_CHECK_LADDER=0 restores the original unbounded diagnostic.'
    exit 0
fi
if [[ "${ATHENA_VISION_CHECK_LADDER:-1}" != 0 ]]; then
    set -uo pipefail
    DEV="${1:-/dev/video0}"
    RES="${2:-1920x1080}"
    [[ "$RES" =~ ^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}$ ]] || {
        echo "FAIL: invalid resolution: $RES" >&2; exit 2;
    }
    [[ -e "$DEV" && -r "$DEV" ]] || { echo "FAIL: camera is absent or unreadable: $DEV" >&2; exit 1; }
    command -v timeout >/dev/null || { echo 'FAIL: coreutils timeout is required.' >&2; exit 2; }
    if command -v ffprobe >/dev/null; then
        VALIDATOR=ffprobe
    elif command -v identify >/dev/null; then
        VALIDATOR=identify
    else
        echo 'FAIL: ffprobe (supplied with ffmpeg) or identify is needed to validate JPEG dimensions.' >&2
        exit 2
    fi
    OUTDIR=$(mktemp -d "${TMPDIR:-/tmp}/athena-vision-check.XXXXXX") || exit 2
    OUT="$OUTDIR/frame.jpg"
    LOG="$OUTDIR/capture.log"
    GOOD=0
    trap 'if [[ "$GOOD" != 1 ]]; then rm -rf -- "$OUTDIR"; fi' EXIT
    echo "Athena vision preflight: $DEV requested $RES"
    if command -v v4l2-ctl >/dev/null; then
        timeout -k 1 5 v4l2-ctl -d "$DEV" --list-formats-ext 2>/dev/null | sed -n '1,25p' || true
    fi
    dims() {
        [[ -s "$OUT" ]] || return 1
        local got
        if [[ "$VALIDATOR" == ffprobe ]]; then
            got=$(timeout -k 1 5 ffprobe -v error -select_streams v:0 \
                -show_entries stream=codec_name,width,height -of csv=p=0:s=x "$OUT" 2>/dev/null) || return 1
            [[ "$got" =~ ^mjpegx([1-9][0-9]*)x([1-9][0-9]*)$ ]] || return 1
            DIMS="${BASH_REMATCH[1]}x${BASH_REMATCH[2]}"
        else
            got=$(timeout -k 1 5 identify -format '%m %w %h' "$OUT" 2>/dev/null) || return 1
            [[ "$got" =~ ^JPEG\ ([1-9][0-9]*)\ ([1-9][0-9]*)$ ]] || return 1
            DIMS="${BASH_REMATCH[1]}x${BASH_REMATCH[2]}"
        fi
    }
    grab() {
        local rung="$1"; shift
        rm -f -- "$OUT"
        printf 'Trying %s\n' "$rung"
        # Like the production ladder, a readable JPEG determines acceptance;
        # a failed encoder must not leave an invalid nonempty file as success.
        timeout -k 1 10 "$@" >> "$LOG" 2>&1 || true
        if dims; then GOOD=1; VIA="$rung"; return 0; fi
        return 1
    }
    if command -v ffmpeg >/dev/null; then
        if [[ "${ATHENA_CAPTURE_MJPEG:-1}" != 0* ]]; then
            codec=(-q:v 2)
            [[ "${ATHENA_CAPTURE_COPY:-0}" == 1* ]] && codec=(-c:v copy -bsf:v mjpeg2jpeg)
            grab mjpeg ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
                -video_size "$RES" -i "$DEV" -frames:v 1 "${codec[@]}" -update 1 -y "$OUT" || true
        fi
        if [[ "$GOOD" != 1 ]]; then
            grab auto ffmpeg -hide_banner -loglevel error -f v4l2 -video_size "$RES" \
                -i "$DEV" -frames:v 1 -update 1 -y "$OUT" || true
        fi
    fi
    if [[ "$GOOD" != 1 ]] && command -v fswebcam >/dev/null; then
        grab fswebcam fswebcam -q -r "$RES" -D 1 --no-banner --jpeg 92 -d "$DEV" "$OUT" || true
    fi
    if [[ "$GOOD" != 1 ]]; then
        echo 'FAIL: no rung produced a readable JPEG; command diagnostics follow.' >&2
        cat "$LOG" >&2
        exit 1
    fi
    echo "OK: capture via $VIA: $DIMS (requested $RES)"
    if [[ "$DIMS" != "$RES" ]]; then
        echo 'WARNING: usable image, but requested resolution was not delivered; S23 resolution preflight is incomplete.'
    fi
    printf 'Frame: %s\nCapture log: %s\n' "$OUT" "$LOG"
    echo 'Run the brain with your usual model flags plus --vision-bench and this frame path to measure a look.'
    exit 0
fi

set -u
DEV="${1:-/dev/video0}"
RES="${2:-1920x1080}"
OUT="/tmp/athena-vision-check.jpg"

echo "== Athena vision preflight =="
echo "device: $DEV   resolution: $RES"

# 1) device exists and is readable
if [ ! -e "$DEV" ]; then
    echo "FAIL: $DEV does not exist. ls /dev/video* to find your camera."
    exit 1
fi
if [ ! -r "$DEV" ]; then
    echo "FAIL: $DEV not readable — add yourself to the video group:"
    echo "      sudo usermod -aG video \$USER   (then re-login)"
    exit 1
fi

# 2) what the camera can actually do (informational)
if command -v v4l2-ctl >/dev/null 2>&1; then
    echo "-- supported formats (v4l2-ctl) --"
    v4l2-ctl -d "$DEV" --list-formats-ext 2>/dev/null | sed -n '1,25p'
else
    echo "(v4l2-ctl not installed — skip; apt install v4l-utils for format listing)"
fi

# 3) the exact grab the live look performs: ffmpeg first, fswebcam fallback
rm -f "$OUT"
if command -v ffmpeg >/dev/null 2>&1; then
    echo "-- grabbing one frame via ffmpeg --"
    ffmpeg -hide_banner -loglevel error -f v4l2 -video_size "$RES" -i "$DEV" \
           -frames:v 1 -update 1 -y "$OUT" 2>/dev/null
fi
if [ ! -s "$OUT" ] && command -v fswebcam >/dev/null 2>&1; then
    echo "-- ffmpeg gave nothing; trying fswebcam --"
    fswebcam -q -r "$RES" -D 1 --no-banner --jpeg 92 -d "$DEV" "$OUT" 2>/dev/null
fi

if [ -s "$OUT" ]; then
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || stat -f%z "$OUT")
    echo "OK: captured $OUT (${SIZE} bytes)"
    if command -v identify >/dev/null 2>&1; then
        identify "$OUT"
    fi
    echo
    echo "Next: measure the real cost of a look on this rig (no audio stack needed):"
    echo "  ./whisper-talk-llama <your usual -m/-ml/-c flags> \\"
    echo "      --mmproj mmproj-BF16.gguf --vision-bench $OUT"
    exit 0
else
    echo "FAIL: no frame captured. Install ffmpeg (or fswebcam), check the device"
    echo "      with 'v4l2-ctl -d $DEV --list-formats-ext', and confirm nothing"
    echo "      else holds the camera open."
    exit 1
fi
