#!/usr/bin/env bash
set -eEuo pipefail

BIN="${OVERLAYD_BIN:-overlayd}"
JOB=J
STORE="$(mktemp -d)"
RUNTIME_BASE=""
BASE_TMP=""
GENERATED_BASE_TAR=""
BASE_TAR="${1:-base.tar}"

cleanup() {
    set +e
    if "$BIN" --root "$STORE" ws info "$JOB" >/dev/null 2>&1; then
        "$BIN" --root "$STORE" ws unmount "$JOB" >/dev/null 2>&1 || true
        "$BIN" --root "$STORE" ws rm "$JOB" >/dev/null 2>&1 || true
    fi
    [[ -n "$BASE_TMP" ]] && rm -rf "$BASE_TMP"
    [[ -n "$GENERATED_BASE_TAR" ]] && rm -f "$GENERATED_BASE_TAR"
    rm -rf "$STORE"
}
trap cleanup EXIT

if [[ ! -w /run ]]; then
    mount -t tmpfs tmpfs /run
fi

install -d -m 0755 "/run/jobs/$JOB"

if [[ ! -f "$BASE_TAR" ]]; then
    BASE_TMP="$(mktemp -d)"
    mkdir -p "$BASE_TMP/usr/bin"
    printf '#!/bin/sh\nexit 0\n' >"$BASE_TMP/usr/bin/placeholder"
    chmod 0755 "$BASE_TMP/usr/bin/placeholder"
    BASE_TAR="$PWD/base.tar"
    GENERATED_BASE_TAR="$BASE_TAR"
    (
        cd "$BASE_TMP"
        tar -cf "$BASE_TAR" usr
    )
fi

"$BIN" --root "$STORE" init
"$BIN" --root "$STORE" layer import "$BASE_TAR" base
MERGED="$("$BIN" --root "$STORE" ws create "$JOB" -l base -m "/run/jobs/$JOB/root")"
test "$MERGED" = "/run/jobs/$JOB/root"





"$BIN" --root "$STORE" ws unmount "$JOB"
"$BIN" --root "$STORE" ws rm "$JOB"
