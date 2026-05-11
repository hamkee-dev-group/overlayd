#!/usr/bin/env bash


set -eEuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/overlayd"

if [[ ! -x "$BIN" ]]; then
    (cd "$ROOT" && make)
fi

WORK="$(mktemp -d -t overlayd-test.XXXXXX)"
RUN_OUT_FILE="$WORK/cmd.out"
RUN_ERR_FILE="$WORK/cmd.err"
export BIN WORK

cleanup() {
    if [[ -d "$WORK/.overlayd/workspaces" ]]; then
        for ws in "$WORK"/.overlayd/workspaces/*/; do
            [[ -d "$ws" ]] || continue
            chmod -R u+rwx "$ws" 2>/dev/null || true
        done
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

cd "$WORK"

step()  { printf '\033[1;36m== %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32mok\033[0m: %s\n' "$*"; }
fail()  { printf '\033[1;31mFAIL\033[0m: %s\n' "$*"; exit 1; }

check_eq() {
    if [[ "$1" != "$2" ]]; then
        fail "expected '$2', got '$1' ($3)"
    fi
}

check_file_empty() {
    [[ ! -s "$1" ]] || fail "$2 should be empty"
}

check_file_nonempty() {
    [[ -s "$1" ]] || fail "$2 should be non-empty"
}

check_file_exact() {
    local file="$1"
    local expected="$2"
    local label="$3"
    cmp -s "$file" <(printf '%s' "$expected") || fail "$label mismatch"
}

check_file_contains() {
    local file="$1"
    local needle="$2"
    local label="$3"
    grep -Fq "$needle" "$file" || fail "$label missing '$needle'"
}

run_capture() {
    : >"$RUN_OUT_FILE"
    : >"$RUN_ERR_FILE"
    set +e
    "$@" >"$RUN_OUT_FILE" 2>"$RUN_ERR_FILE"
    RUN_RC=$?
    set -e
}

assert_failure_contract() {
    local expected_rc="$1"
    local label="$2"
    shift 2
    run_capture "$@"
    check_eq "$RUN_RC" "$expected_rc" "$label rc"
    check_file_empty "$RUN_OUT_FILE" "$label stdout"
    check_file_nonempty "$RUN_ERR_FILE" "$label stderr"
}

assert_success_line() {
    local expected="$1"
    local label="$2"
    shift 2
    run_capture "$@"
    check_eq "$RUN_RC" "0" "$label rc"
    check_file_exact "$RUN_OUT_FILE" "$expected"$'\n' "$label stdout"
    check_file_empty "$RUN_ERR_FILE" "$label stderr"
}

assert_success_silent() {
    local label="$1"
    shift
    run_capture "$@"
    check_eq "$RUN_RC" "0" "$label rc"
    check_file_empty "$RUN_OUT_FILE" "$label stdout"
    check_file_empty "$RUN_ERR_FILE" "$label stderr"
}

make_tar_fixture() {
    local kind="$1"
    local tar_path="$2"
    shift 2
    python3 - "$kind" "$tar_path" "$@" <<'PY'
import io
import os
import sys
import tarfile

kind = sys.argv[1]
tar_path = sys.argv[2]
args = sys.argv[3:]

if kind == "regular":
    member, payload = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "gnu_long":
    member, payload = args
    fmt = tarfile.GNU_FORMAT
elif kind == "pax_path":
    member, payload = args
    fmt = tarfile.PAX_FORMAT
elif kind == "symlink_same":
    link_name, target, file_name, payload = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "symlink_parent":
    link_name, target, file_name, payload = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "raw_corrupt":
    member, payload = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "bad_magic":
    member, payload = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "whiteout":
    keep_name, keep_payload, wh_name = args
    fmt = tarfile.USTAR_FORMAT
elif kind == "raw_longlink_huge":
    hdr = bytearray(512)
    name = b"././@LongLink"
    hdr[0:len(name)] = name
    hdr[100:108] = b"0000644\x00"
    hdr[108:116] = b"0000000\x00"
    hdr[116:124] = b"0000000\x00"
    hdr[124:136] = b"77777777777\x00"
    hdr[136:148] = b"00000000000\x00"
    hdr[148:156] = b"        "
    hdr[156] = ord('L')
    hdr[257:263] = b"ustar "
    hdr[263:265] = b" \x00"
    s = sum(hdr)
    hdr[148:154] = ("%06o" % s).encode()
    hdr[154] = 0
    hdr[155] = ord(' ')
    with open(tar_path, "wb") as f:
        f.write(bytes(hdr))
    sys.exit(0)
else:
    raise SystemExit(f"unknown fixture kind: {kind}")

with tarfile.open(tar_path, "w", format=fmt, dereference=False) as tf:
    if kind in {"regular", "gnu_long"}:
        ti = tarfile.TarInfo(member)
        data = payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(data))
    elif kind == "pax_path":
        ti = tarfile.TarInfo("placeholder")
        data = payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        ti.pax_headers = {"path": member}
        tf.addfile(ti, io.BytesIO(data))
    elif kind == "symlink_same":
        ti = tarfile.TarInfo(link_name)
        ti.type = tarfile.SYMTYPE
        ti.linkname = target
        tf.addfile(ti)

        ti = tarfile.TarInfo(file_name)
        data = payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(data))
    elif kind == "symlink_parent":
        ti = tarfile.TarInfo(link_name)
        ti.type = tarfile.SYMTYPE
        ti.linkname = target
        tf.addfile(ti)

        ti = tarfile.TarInfo(file_name)
        data = payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(data))
    elif kind in {"raw_corrupt", "bad_magic"}:
        ti = tarfile.TarInfo(member)
        data = payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(data))
    elif kind == "whiteout":
        ti = tarfile.TarInfo(keep_name)
        data = keep_payload.encode()
        ti.size = len(data)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(data))
        ti = tarfile.TarInfo(wh_name)
        ti.size = 0
        ti.mode = 0o644
        tf.addfile(ti)

if kind == "raw_corrupt":
    with open(tar_path, "r+b") as f:
        f.seek(148)
        b = bytearray(f.read(1))
        b[0] ^= 0x01
        f.seek(148)
        f.write(bytes(b))
elif kind == "bad_magic":
    with open(tar_path, "r+b") as f:
        f.seek(257)
        f.write(b"XXXXXX")
PY
}

assert_import_rejected() {
    local tar_path="$1"
    local layer_name="$2"
    shift 2
    assert_failure_contract 1 "layer import $layer_name rejected" \
        "$BIN" layer import "$tar_path" "$layer_name"
    [[ ! -e ".overlayd/layers/$layer_name" ]] || fail "rejected layer $layer_name created"
    local left
    left=$(find ./.overlayd/layers -maxdepth 1 -name ".tmp.$layer_name.*" | wc -l)
    check_eq "$left" "0" "no temp dirs for $layer_name"
    for outside in "$@"; do
        [[ ! -e "$outside" ]] || fail "unsafe import wrote $outside"
    done
}

step "init"
out=$("$BIN" init)
[[ "$out" == "./.overlayd" ]] || fail "init output: $out"
[[ -d ./.overlayd/layers ]] || fail "layers dir not created"
[[ -d ./.overlayd/workspaces ]] || fail "workspaces dir not created"
ok "store initialized"

step "init is idempotent"
"$BIN" init >/dev/null
ok "re-init succeeded"

step "help documents info and materialize"
help_text=$("$BIN" help 2>&1)
[[ "$help_text" == *"layer create|list|rm|path|info|commit|import|export"* ]] || fail "help missing layer info"
[[ "$help_text" == *"ws create|list|info|path|upper|mount|unmount|rm"* ]] || fail "help missing ws info"
[[ "$help_text" == *"materialize <name> -l L1 [-l L2 ...] -m PATH"* ]] || fail "help missing materialize"
ok "help output advertises cli surface"

step "dispatch usage errors"
assert_failure_contract 2 "overlayd no command" "$BIN"
assert_failure_contract 2 "overlayd unknown command" "$BIN" nope
assert_failure_contract 2 "overlayd layer missing subcommand" "$BIN" layer
assert_failure_contract 2 "overlayd layer unknown subcommand" "$BIN" layer nope
assert_failure_contract 2 "overlayd ws missing subcommand" "$BIN" ws
assert_failure_contract 2 "overlayd ws unknown subcommand" "$BIN" ws nope
ok "usage paths are stderr-only rc=2"

step "exit code contract"
assert_failure_contract 2 "missing --root value" "$BIN" --root
assert_failure_contract 2 "ws create missing name" "$BIN" ws create
assert_failure_contract 2 "ws create missing layer" "$BIN" ws create foo
assert_failure_contract 2 "invalid layer name create" "$BIN" layer create ../bad
assert_failure_contract 2 "invalid slash name create" "$BIN" layer create a/b
assert_failure_contract 2 "ws unknown subcommand" "$BIN" ws unknownsub
ok "usage errors stay on rc=2"

step "layer create / list / path"
assert_success_line "patch" "layer create patch" "$BIN" layer create patch
assert_success_line "base" "layer create base" "$BIN" layer create base
layer_list=$("$BIN" layer list)
first_name=$(printf '%s\n' "$layer_list" | sed -n '1s/\t.*//p')
second_name=$(printf '%s\n' "$layer_list" | sed -n '2s/\t.*//p')
check_eq "$first_name" "base" "sorted layer list first entry"
check_eq "$second_name" "patch" "sorted layer list second entry"
IFS=$'\t' read -r layer_name layer_created layer_extra <<<"$(printf '%s\n' "$layer_list" | sed -n '1p')"
[[ -n "$layer_created" ]] || fail "layer list missing created_at"
[[ -z "${layer_extra:-}" ]] || fail "layer list has extra columns"
base_path=$("$BIN" layer path base)
[[ "$base_path" = */layers/base/content ]] || fail "layer path: $base_path"
echo "hello-from-base" >"$base_path/hello.txt"
mkdir -p "$base_path/dir1" "$base_path/usr/bin"
echo "in-dir" >"$base_path/dir1/file.txt"
echo "placeholder" >"$base_path/usr/bin/placeholder"
ln -s hello.txt "$base_path/symlink"
patch_path=$("$BIN" layer path patch)
mkdir -p "$patch_path/dir1"
echo "patched-content" >"$patch_path/dir1/file.txt"
ok "layers created with deterministic list output"

step "invalid names rejected"
assert_failure_contract 2 "invalid layer name ../bad" "$BIN" layer create ../bad
assert_failure_contract 2 "invalid empty layer name" "$BIN" layer create ""
assert_failure_contract 2 "invalid layer name a/b" "$BIN" layer create a/b
ok "write paths validate names"

step "duplicate layer fails"
assert_failure_contract 1 "duplicate layer create" "$BIN" layer create base
ok "duplicate layer rejected"

step "ws create without mount formalizes materialization path"
dev_mp="$(realpath -m "$WORK/run/dev-root")"
aaa_mp="$(realpath -m "$WORK/run/aaa-root")"
export dev_mp aaa_mp
assert_success_line "$dev_mp" "ws create dev" "$BIN" ws create dev -l patch -l base --no-mount -m "$WORK/run/dev-root"
[[ -d "$dev_mp" ]] || fail "dev mountpoint not created"
assert_success_line "$dev_mp" "ws path dev" "$BIN" ws path dev
dev_upper_expected="$(realpath "$WORK/.overlayd/workspaces/dev/upper")"
assert_success_line "$dev_upper_expected" "ws upper dev" "$BIN" ws upper dev
assert_success_line "$aaa_mp" "ws create aaa" "$BIN" ws create aaa -l base --no-mount -m "$WORK/run/aaa-root"
ws_list=$("$BIN" ws list)
first_ws=$(printf '%s\n' "$ws_list" | sed -n '1s/\t.*//p')
second_ws=$(printf '%s\n' "$ws_list" | sed -n '2s/\t.*//p')
check_eq "$first_ws" "aaa" "sorted ws list first entry"
check_eq "$second_ws" "dev" "sorted ws list second entry"
dev_row=$(printf '%s\n' "$ws_list" | awk -F '\t' '$1 == "dev" { print; exit }')
IFS=$'\t' read -r ws_name ws_state ws_mp ws_lowers ws_extra <<<"$dev_row"
check_eq "$ws_name" "dev" "ws list name column"
check_eq "$ws_state" "unmounted" "ws list state column"
check_eq "$ws_mp" "$dev_mp" "ws list mountpoint column"
check_eq "$ws_lowers" "patch,base" "ws list lowers column"
[[ -z "${ws_extra:-}" ]] || fail "ws list has extra columns"
ok "workspace create/path/upper/list contract holds"

step "read-only queries distinguish invalid from missing"
assert_failure_contract 2 "layer info invalid" "$BIN" layer info ../bad
check_file_contains "$RUN_ERR_FILE" "invalid layer name" "layer info invalid stderr"
assert_failure_contract 2 "layer path invalid" "$BIN" layer path ../bad
check_file_contains "$RUN_ERR_FILE" "invalid layer name" "layer path invalid stderr"
assert_failure_contract 1 "layer info missing" "$BIN" layer info missing
check_file_contains "$RUN_ERR_FILE" "no such layer" "layer info missing stderr"
assert_failure_contract 1 "layer path missing" "$BIN" layer path missing
check_file_contains "$RUN_ERR_FILE" "no such layer" "layer path missing stderr"
assert_failure_contract 2 "ws info invalid" "$BIN" ws info ../bad
check_file_contains "$RUN_ERR_FILE" "invalid workspace name" "ws info invalid stderr"
assert_failure_contract 2 "ws path invalid" "$BIN" ws path ../bad
check_file_contains "$RUN_ERR_FILE" "invalid workspace name" "ws path invalid stderr"
assert_failure_contract 2 "ws upper invalid" "$BIN" ws upper ../bad
check_file_contains "$RUN_ERR_FILE" "invalid workspace name" "ws upper invalid stderr"
assert_failure_contract 2 "ws mount invalid" "$BIN" ws mount ../bad
check_file_contains "$RUN_ERR_FILE" "invalid workspace name" "ws mount invalid stderr"
assert_failure_contract 2 "ws unmount invalid" "$BIN" ws unmount ../bad
check_file_contains "$RUN_ERR_FILE" "invalid workspace name" "ws unmount invalid stderr"
assert_failure_contract 1 "ws info missing" "$BIN" ws info missing
check_file_contains "$RUN_ERR_FILE" "no such workspace" "ws info missing stderr"
assert_failure_contract 1 "ws path missing" "$BIN" ws path missing
check_file_contains "$RUN_ERR_FILE" "no such workspace" "ws path missing stderr"
assert_failure_contract 1 "ws upper missing" "$BIN" ws upper missing
check_file_contains "$RUN_ERR_FILE" "no such workspace" "ws upper missing stderr"
ok "read-only queries keep rc=2 for invalid and rc=1 for missing"

step "failure paths produce empty stdout"
assert_failure_contract 1 "layer create existing base" "$BIN" layer create base
assert_failure_contract 1 "ws create existing dev" "$BIN" ws create dev -l base
assert_failure_contract 1 "ws path missing stdout" "$BIN" ws path nope
assert_failure_contract 1 "ws upper missing stdout" "$BIN" ws upper nope
assert_failure_contract 1 "ws info missing stdout" "$BIN" ws info nope
assert_failure_contract 1 "layer path missing stdout" "$BIN" layer path nope
assert_failure_contract 1 "layer info missing stdout" "$BIN" layer info nope
ok "all checked failures stay off stdout"

step "layer rm refused while in use"
assert_failure_contract 1 "layer rm in use" "$BIN" layer rm base
ok "in-use protection works"

step "layer info / ws info"
out=$("$BIN" layer info base)
[[ "$out" == *"name=base"* ]] || fail "layer info missing name"
[[ "$out" == *"created_at="* ]] || fail "layer info missing timestamp"
out=$("$BIN" ws info dev)
[[ "$out" == *"lowers=patch,base"* ]] || fail "ws info missing lowers"
[[ "$out" == *"mounted=0"* ]] || fail "ws info wrong mount state"
ok "info commands return metadata"

step "layer create refuses pre-staged tmp directory"
: >"$RUN_OUT_FILE"
: >"$RUN_ERR_FILE"
set +e
bash -c '
    set -eu
    mkdir -p "$WORK/.overlayd/layers/.tmp.collision.$BASHPID/content"
    echo stale >"$WORK/.overlayd/layers/.tmp.collision.$BASHPID/content/stale.txt"
    cd "$WORK"
    exec "$BIN" layer create collision
' >"$RUN_OUT_FILE" 2>"$RUN_ERR_FILE"
RUN_RC=$?
set -e
[[ "$RUN_RC" -ne 0 ]] || fail "collision layer create rc"
check_file_empty "$RUN_OUT_FILE" "collision layer create stdout"
check_file_nonempty "$RUN_ERR_FILE" "collision layer create stderr"
[[ ! -e "$WORK/.overlayd/layers/collision" ]] || fail "collision layer published"
rm -rf "$WORK"/.overlayd/layers/.tmp.collision.*
ok "pre-existing tmp dir is rejected without publishing"

step "atomic layer create leaves no temp dirs"
assert_success_line "staged" "layer create staged" "$BIN" layer create staged
assert_failure_contract 1 "layer create staged duplicate" "$BIN" layer create staged
assert_success_silent "layer rm staged" "$BIN" layer rm staged
left=$(find ./.overlayd/layers -maxdepth 1 -name '.tmp.*' | wc -l)
check_eq "$left" "0" "no .tmp leftovers"
ok "temp layer dirs are cleaned up"

step "layer export / import stdout contract and roundtrip"
assert_success_silent "layer export base" "$BIN" layer export base "$WORK/base.tar"
[[ -s "$WORK/base.tar" ]] || fail "tar empty"
assert_failure_contract 1 "layer export missing" "$BIN" layer export missing "$WORK/missing.tar"
assert_success_line "imported" "layer import imported" "$BIN" layer import "$WORK/base.tar" imported
assert_failure_contract 1 "layer import duplicate" "$BIN" layer import "$WORK/base.tar" imported
imported_path=$("$BIN" layer path imported)
diff -r "$base_path" "$imported_path" >/dev/null || fail "tar roundtrip diff"
ok "export/import contracts are pinned"

step "mount, idempotence, and overlay writes"
unshare -Urm bash <<'EOF'
set -euo pipefail
cd "$WORK"
count_mounts() {
    awk -v mp="$dev_mp" '$5 == mp && $9 == "overlay" { n++ } END { print n + 0 }' /proc/self/mountinfo
}
"$BIN" ws mount dev >/tmp/dev.mount.out 2>/tmp/dev.mount.err
test ! -s /tmp/dev.mount.out
test ! -s /tmp/dev.mount.err
test "$(count_mounts)" = 1
"$BIN" ws mount dev
test "$(count_mounts)" = 1
test "$("$BIN" ws info dev | awk -F= '$1=="mounted"{print $2}')" = "1"
test "$(cat "$dev_mp/hello.txt")" = "hello-from-base"
test "$(cat "$dev_mp/dir1/file.txt")" = "patched-content"
echo 'wrote-via-overlay' >"$dev_mp/new.txt"
rm "$dev_mp/hello.txt"
"$BIN" ws unmount dev >/tmp/dev.umount.out 2>/tmp/dev.umount.err
test ! -s /tmp/dev.umount.out
test ! -s /tmp/dev.umount.err
EOF
[[ -f ./.overlayd/workspaces/dev/upper/new.txt ]] || fail "upper missing new.txt"
[[ -e ./.overlayd/workspaces/dev/upper/hello.txt ]] || fail "whiteout not present"
[[ -c ./.overlayd/workspaces/dev/upper/hello.txt ]] || fail "whiteout not char dev"
ok "mount is idempotent and overlay writes land in upper"

step "materialize alias"
mat_mp="$(realpath -m "$WORK/run/jobs/J/root")"
export mat_mp
unshare -Urm bash <<'EOF'
set -euo pipefail
cd "$WORK"
out="$("$BIN" materialize J -l base -m "$WORK/run/jobs/J/root")"
test "$out" = "$mat_mp"
test "$("$BIN" ws path J)" = "$mat_mp"
test "$("$BIN" ws info J | awk -F= '$1=="mounted"{print $2}')" = "1"
"$BIN" ws unmount J
"$BIN" ws rm J
EOF
ok "materialize wraps create+mount with a single path output"

step "commit workspace upper into a new layer"
assert_success_line "v2" "layer commit dev v2" "$BIN" layer commit dev v2
assert_failure_contract 1 "layer commit duplicate target" "$BIN" layer commit dev v2
v2_path=$("$BIN" layer path v2)
[[ -f "$v2_path/new.txt" ]] || fail "v2 missing new.txt"
[[ -c "$v2_path/hello.txt" ]] || fail "v2 missing whiteout"
ok "commit preserves upper contents and stdout contract"

step "stack committed layer back over base"
assert_success_line "$(realpath -m "$WORK/run/verify-root")" "ws create verify" \
    "$BIN" ws create verify -l v2 -l base --no-mount -m "$WORK/run/verify-root"
unshare -Urm bash <<'EOF'
set -euo pipefail
cd "$WORK"
"$BIN" ws mount verify
test ! -e "$WORK/run/verify-root/hello.txt"
test "$(cat "$WORK/run/verify-root/new.txt")" = "wrote-via-overlay"
test -d "$WORK/run/verify-root/dir1"
"$BIN" ws unmount verify
EOF
ok "committed layer replays correctly"

step "stale mounted metadata is reconciled"
sed -i 's/^mounted=0$/mounted=1/' ./.overlayd/workspaces/aaa/meta
assert_success_silent "ws unmount aaa with stale mounted=1" "$BIN" ws unmount aaa
[[ "$("$BIN" ws info aaa | awk -F= '$1=="mounted"{print $2}')" == "0" ]] || fail "stale unmount did not reset metadata"
unshare -Urm bash <<'EOF'
set -euo pipefail
cd "$WORK"
"$BIN" ws create stale -l base -m "$WORK/run/stale-root" >/dev/null
sed -i 's/^mounted=1$/mounted=0/' ./.overlayd/workspaces/stale/meta
"$BIN" ws list | awk -F '\t' '$1=="stale"{print $2}' | grep -Fxq mounted
"$BIN" ws rm stale
test ! -e ./.overlayd/workspaces/stale
EOF
ok "real mount state wins over stale metadata"

step "ws rm --force refuses to delete through a live mount"
unshare -Urm bash <<'EOF'
set -euo pipefail
cd "$WORK"
"$BIN" ws create busy -l base -m "$WORK/run/busy-root" >/dev/null
exec 9<"$WORK/run/busy-root"
set +e
"$BIN" ws rm --force busy >/tmp/busy.rm.out 2>/tmp/busy.rm.err
rc=$?
set -e
test "$rc" -ne 0
test -d ./.overlayd/workspaces/busy
test -d ./.overlayd/workspaces/busy/upper
mountpoint -q "$WORK/run/busy-root"
exec 9<&-
"$BIN" ws unmount busy
"$BIN" ws rm busy
EOF
ok "force remove stops when unmount leaves the workspace live"

step "silent ws rm for an unmounted workspace"
assert_success_silent "ws rm aaa" "$BIN" ws rm aaa
ok "ws rm is silent on success"

step "unsafe tar imports are rejected"
outside_dir="$WORK/outside"
mkdir -p "$outside_dir"

make_tar_fixture regular "$WORK/evil_traverse.tar" "../pwned.txt" "pwned"
assert_import_rejected "$WORK/evil_traverse.tar" evil_traverse "$WORK/pwned.txt" "$PWD/pwned.txt"

make_tar_fixture regular "$WORK/evil_nested.tar" "dir/../../outside.txt" "pwned"
assert_import_rejected "$WORK/evil_nested.tar" evil_nested "$WORK/outside.txt" "$PWD/outside.txt"

make_tar_fixture regular "$WORK/evil_abs.tar" "/absolute/path.txt" "pwned"
assert_import_rejected "$WORK/evil_abs.tar" evil_abs "$WORK/absolute/path.txt"

long_traverse=$(printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/../outside-long.txt')
make_tar_fixture gnu_long "$WORK/evil_long_traverse.tar" "$long_traverse" "pwned"
assert_import_rejected "$WORK/evil_long_traverse.tar" evil_long_traverse "$WORK/outside-long.txt" "$PWD/outside-long.txt"

long_abs="/$(printf 'a%.0s' {1..120})/evil.txt"
make_tar_fixture gnu_long "$WORK/evil_long_abs.tar" "$long_abs" "pwned"
assert_import_rejected "$WORK/evil_long_abs.tar" evil_long_abs

make_tar_fixture pax_path "$WORK/evil_pax_rel.tar" "../pax-outside.txt" "pwned"
assert_import_rejected "$WORK/evil_pax_rel.tar" evil_pax_rel "$WORK/pax-outside.txt" "$PWD/pax-outside.txt"

make_tar_fixture pax_path "$WORK/evil_pax_abs.tar" "/pax/absolute.txt" "pwned"
assert_import_rejected "$WORK/evil_pax_abs.tar" evil_pax_abs

symlink_target_same="/tmp/overlayd-pwn-same-$$"
mkdir -p "$symlink_target_same"
make_tar_fixture symlink_same "$WORK/evil_symlink_same.tar" "evil" "$symlink_target_same/pwn.txt" "evil" "pwned"
assert_import_rejected "$WORK/evil_symlink_same.tar" evil_symlink_same "$symlink_target_same/pwn.txt"
rm -rf "$symlink_target_same"

symlink_target_parent="/tmp/overlayd-pwn-parent-$$"
mkdir -p "$symlink_target_parent"
make_tar_fixture symlink_parent "$WORK/evil_symlink_parent.tar" "evil" "$symlink_target_parent" "evil/pwn.txt" "pwned"
assert_import_rejected "$WORK/evil_symlink_parent.tar" evil_symlink_parent "$symlink_target_parent/pwn.txt"
rm -rf "$symlink_target_parent"

make_tar_fixture raw_corrupt "$WORK/evil_chksum.tar" "data.txt" "pwned"
assert_import_rejected "$WORK/evil_chksum.tar" evil_chksum

make_tar_fixture bad_magic "$WORK/evil_magic.tar" "data.txt" "pwned"
assert_import_rejected "$WORK/evil_magic.tar" evil_magic

make_tar_fixture raw_longlink_huge "$WORK/evil_longlink_huge.tar"
assert_import_rejected "$WORK/evil_longlink_huge.tar" evil_longlink_huge

ok "import path traversal and symlink escapes are blocked"

step "import converts docker .wh.* markers to overlay whiteouts"
make_tar_fixture whiteout "$WORK/wh.tar" "keep.txt" "alive" ".wh.gone.txt"
assert_success_line "wh" "layer import wh" "$BIN" layer import "$WORK/wh.tar" wh
wh_path=$("$BIN" layer path wh)
[[ -f "$wh_path/keep.txt" ]] || fail "keep.txt missing"
[[ "$(cat "$wh_path/keep.txt")" == "alive" ]] || fail "keep.txt content"
[[ ! -e "$wh_path/.wh.gone.txt" ]] || fail ".wh.gone.txt left in layer content"
[[ -e "$wh_path/gone.txt" ]] || fail "gone.txt whiteout missing"
[[ -c "$wh_path/gone.txt" ]] || fail "gone.txt not a char device"
[[ "$(stat -c '%F' "$wh_path/gone.txt")" == "character special file" ]] || fail "gone.txt %F"
[[ "$(stat -c '%t %T' "$wh_path/gone.txt")" == "0 0" ]] || fail "gone.txt not 0/0"
ok ".wh.* markers become overlay char-dev whiteouts"

step "documentation guard"
[[ -f "$ROOT/README.md" ]] || fail "README.md missing"
grep -Fq "sandbox --prepare-only" "$ROOT/README.md" || fail "README missing sandbox --prepare-only"
grep -Fq "landlockd" "$ROOT/README.md" || fail "README missing landlockd"
grep -Fq "cgroupd wait" "$ROOT/README.md" || fail "README missing cgroupd wait"
grep -Fq "/run/jobs/J/root" "$ROOT/README.md" || fail "README missing /run/jobs/J/root"
grep -Fq "ws unmount J" "$ROOT/README.md" || fail "README missing ws unmount J"
ok "README covers the orchestration flow"

step "workspace cleanup"
assert_success_silent "ws rm dev" "$BIN" ws rm dev
assert_success_silent "ws rm verify" "$BIN" ws rm verify
list=$("$BIN" ws list | wc -l)
check_eq "$list" "0" "ws list count"
ok "workspaces removed cleanly"

step "layer cleanup"
assert_success_silent "layer rm patch" "$BIN" layer rm patch
assert_success_silent "layer rm imported" "$BIN" layer rm imported
assert_success_silent "layer rm v2" "$BIN" layer rm v2
assert_success_silent "layer rm wh" "$BIN" layer rm wh
assert_success_silent "layer rm base" "$BIN" layer rm base
list=$("$BIN" layer list | wc -l)
check_eq "$list" "0" "layer list count"
ok "all layers removed"

step "no leftovers in store"
left=$(find .overlayd/layers .overlayd/workspaces -mindepth 1 -maxdepth 1 | wc -l)
check_eq "$left" "0" "store leftovers"
ok "store is clean"

printf '\n\033[1;32mAll tests passed.\033[0m\n'
