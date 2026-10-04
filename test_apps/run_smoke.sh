#!/usr/bin/env bash
# Build test_apps/smoke for each chip and run it in esp-emu.
#
# A standalone counterpart of tests/test_app_smoke.rs for places without the
# Rust harness (the public release mirror runs it against every published
# esp-emu binary with the ESP-IDF master docker image). Pass = the app's
# final `RESULT_SUMMARY:` line reports failed=0.
#
# Usage:
#   test_apps/run_smoke.sh [--esp-emu PATH] [--chips esp32c3,esp32c6,...]
#                          [--idf-docker IMAGE | --idf-path DIR]
#                          [--timeout SECS] [--out DIR]
#
# Defaults: esp-emu from PATH ($ESP_EMU), docker image espressif/idf:latest
# ($ESP_EMU_IDF_DOCKER; $IDF_PATH is used instead when set and no image is
# given), every chip this IDF and esp-emu both know, 240 s per chip, ./out.
# A requested chip the IDF cannot build for is reported as SKIP, not FAIL.
#
# Needs: esp-emu >= 0.46 (--no-panic-intercept: the app's watchdog phase
# panics on purpose and must reboot), docker or a sourced ESP-IDF, rsync.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/smoke"
ALL_CHIPS=(esp32c3 esp32c5 esp32c6 esp32h2 esp32p4 esp32s3 esp32s31)

EMU="${ESP_EMU:-esp-emu}"
IMAGE="${ESP_EMU_IDF_DOCKER:-}"
IDF_DIR="${IDF_PATH:-}"
CHIPS=""
TIMEOUT=240
OUT="$PWD/out"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --esp-emu)    EMU="$2"; shift 2 ;;
        --chips)      CHIPS="${2//,/ }"; shift 2 ;;
        --idf-docker) IMAGE="$2"; IDF_DIR=""; shift 2 ;;
        --idf-path)   IDF_DIR="$2"; IMAGE=""; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --out)        OUT="$2"; shift 2 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$IMAGE" || -n "$IDF_DIR" ]] || IMAGE="espressif/idf:latest"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"

# Run an idf.py command line, in docker (repo path bind-mounted as itself, like
# tests/common/idf_app.rs does) or against a sourced ESP-IDF.
idf() {
    if [[ -n "$IMAGE" ]]; then
        docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
            -v "$OUT:$OUT" -w "$OUT" "$IMAGE" bash -c "$1"
    else
        bash -c "source '$IDF_DIR/export.sh' >/dev/null 2>&1 && $1"
    fi
}

command -v "$EMU" >/dev/null || { echo "esp-emu not found: $EMU" >&2; exit 2; }
"$EMU" --help 2>/dev/null | grep -q -- '--no-panic-intercept' \
    || { echo "$EMU is too old: needs --no-panic-intercept (esp-emu >= 0.46)" >&2; exit 2; }

echo "esp-emu: $("$EMU" --version 2>/dev/null | head -1)"
echo "IDF:     $(idf 'idf.py --version' 2>/dev/null | tail -1)"
SUPPORTED=" $(idf 'idf.py --preview --list-targets' 2>/dev/null | tr '\n' ' ') "
[[ -n "$CHIPS" ]] || CHIPS="${ALL_CHIPS[*]}"

declare -A RESULT
failed=0
for chip in $CHIPS; do
    if [[ "$SUPPORTED" != *" $chip "* ]]; then
        RESULT[$chip]="SKIP (not a target of this IDF)"; continue
    fi
    dir="$OUT/smoke-$chip"
    rsync -a --delete --exclude=build/ --exclude=sdkconfig --exclude=sdkconfig.old \
        --exclude=managed_components/ --exclude=dependencies.lock "$APP/" "$dir/"
    echo "=== $chip: building"
    if ! idf "idf.py -C '$dir' --preview set-target $chip >'$dir/build.log' 2>&1 && \
              idf.py -C '$dir' build >>'$dir/build.log' 2>&1 && \
              idf.py -C '$dir' merge-bin -o '$dir/build/merged.bin' >>'$dir/build.log' 2>&1"; then
        RESULT[$chip]="FAIL (build; see $dir/build.log)"; failed=1
        tail -20 "$dir/build.log"; continue
    fi
    echo "=== $chip: running"
    "$EMU" --chip "$chip" --firmware "$dir/build/merged.bin" --elf "$dir/build/emu_test.elf" \
        --no-panic-intercept --timeout "${TIMEOUT}s" --exit-on 'RESULT_SUMMARY:' \
        > "$dir/emu.log" 2>&1 || true
    summary="$(grep -o 'RESULT_SUMMARY: .*' "$dir/emu.log" | tail -1)"
    if [[ "$summary" == *"failed=0"* ]]; then
        RESULT[$chip]="PASS ($summary)"
    else
        RESULT[$chip]="FAIL (${summary:-no RESULT_SUMMARY within ${TIMEOUT}s}; see $dir/emu.log)"; failed=1
        grep -E "FAIL|Firmware abort|Guru|Timeout reached" "$dir/emu.log" | tail -10
    fi
done

echo
echo "smoke results:"
for chip in $CHIPS; do printf '  %-9s %s\n' "$chip" "${RESULT[$chip]}"; done
exit $failed
