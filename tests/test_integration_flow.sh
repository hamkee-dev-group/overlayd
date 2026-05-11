#!/usr/bin/env bash
set -eEuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/overlayd"

if [[ ! -x "$BIN" ]]; then
    (cd "$ROOT" && make)
fi

WORK="$(mktemp -d -t overlayd-flow.XXXXXX)"
export BIN WORK
trap 'rm -rf "$WORK"' EXIT

unshare -Urm bash <<'EOF'
set -eEuo pipefail
cd "$WORK"

"$BIN" init >/dev/null
"$BIN" layer create base >/dev/null
base_path="$("$BIN" layer path base)"
mkdir -p "$base_path/usr/bin"
printf 'placeholder\n' >"$base_path/usr/bin/placeholder"

mountpoint="$WORK/run/jobs/J/root"
expected="$(realpath -m "$mountpoint")"
merged="$("$BIN" ws create J -l base -m "$mountpoint")"
test "$merged" = "$expected"
test "$("$BIN" ws path J)" = "$expected"
test "$(cat "$expected/usr/bin/placeholder")" = "placeholder"

printf 'artifact\n' >"$expected/output.txt"
upper="$("$BIN" ws upper J)"
test "$(cat "$upper/output.txt")" = "artifact"

"$BIN" ws unmount J
"$BIN" ws rm J
test "$(find ./.overlayd/workspaces -mindepth 1 -maxdepth 1 | wc -l)" = 0
EOF
