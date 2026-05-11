# overlayd

`overlayd` is a small C command line tool that turns Linux overlayfs into a
simple image and workspace layering service.

The idea is intentionally narrow: Linux already has a production-grade
filesystem layering primitive, so `overlayd` does not try to reimplement image
layers in user space. It gives you a tiny store, immutable layers, writable
workspaces, tar import/export, and machine-readable paths that other runtime
tools can plug into.

It is useful when you want the practical parts of container-style layering
without taking on a full container runtime, image daemon, or higher-level build
system.

## What It Does

`overlayd` manages two main objects:

- **Layers** are immutable directory trees stored under the overlayd store. A
  layer can be created empty, imported from a tar archive, exported back to tar,
  or committed from a workspace's writable changes.
- **Workspaces** are overlayfs mounts built from one or more layers plus a
  writable upper directory. A workspace gives a job or tool a merged filesystem
  view while keeping its writes separate from the base layers.

That makes the project a good fit for job runners, sandboxes, build workers,
experiments, and local orchestration systems that need repeatable filesystem
state but do not need Docker, containerd, or Nix-style infrastructure.

## Why This Exists

Many systems need the same basic workflow:

1. Start from a known filesystem tree.
2. Give a job a writable view of that tree.
3. Keep the job's changes isolated.
4. Optionally turn those changes into a new reusable layer.
5. Hand exact paths to sandboxing, policy, or monitoring tools.

The kernel already knows how to do the hard filesystem part. `overlayd` is the
thin control plane around that kernel feature.

It is deliberately not a container runtime. It does not manage processes,
networking, cgroups, seccomp, image registries, or package resolution. Those
pieces can live in other tools. `overlayd` focuses on the filesystem layer and
prints stable paths so the rest of your stack can compose around it.

## Features

- Minimal C implementation with no daemon required.
- Store initialization under `./.overlayd`, `$OVERLAYD_ROOT`, or `--root`.
- Immutable layer create/list/remove/info/path operations.
- Tar import and export for moving layer contents in and out.
- Writable workspaces backed by overlayfs `upperdir` and `workdir`.
- Multi-layer workspace creation with ordered lower layers.
- Commit workspace changes into a new layer.
- Machine-readable path commands for orchestration scripts.
- Explicit privilege model documented in [docs/privileges.md](docs/privileges.md).

## Build

```sh
make
make install PREFIX=/usr/local
```

For local testing, you can run the built binary directly:

```sh
./overlayd version
```

## Quickstart

Create a store, add a base layer, create a workspace, inspect its paths, then
clean it up:

```sh
./overlayd init
./overlayd layer create base
./overlayd ws create dev -l base --no-mount
./overlayd ws path dev
./overlayd ws upper dev
./overlayd ws rm dev
```

To mount the workspace, the process needs mount privileges. On systems that
allow overlayfs inside user namespaces, this can be done rootlessly inside a
single user and mount namespace:

```sh
unshare -Urm sh -c '
  ./overlayd ws mount dev
  ./overlayd ws path dev
  ./overlayd ws unmount dev
'
```

In real orchestration code, keep the mount, the workload, and cleanup in the
same mount namespace when using rootless mode.

## Importing And Exporting Layers

Layers can be seeded from tar archives:

```sh
./overlayd init
./overlayd layer import ./base.tar base
./overlayd layer list
./overlayd layer path base
```

And exported again:

```sh
./overlayd layer export base ./base-out.tar
```

You can also commit the writable part of an unmounted workspace into a new
layer:

```sh
./overlayd ws create dev -l base --no-mount
UPPER="$(./overlayd ws upper dev)"
printf 'hello\n' > "$UPPER/example.txt"
./overlayd layer commit dev changed
```

## Machine-Readable Paths

`overlayd` is designed to be used by shell scripts and supervisors. Path
commands print only the path, which makes them safe to capture:

```sh
WS_ROOT="$(sudo ./overlayd --root /var/lib/overlayd ws path J)"
WS_UPPER="$(sudo ./overlayd --root /var/lib/overlayd ws upper J)"
```

Typical uses:

- pass `WS_ROOT` to a sandbox runtime as the filesystem root
- pass `WS_ROOT` to Landlock or another policy engine
- watch `WS_UPPER` with fanotify or export rules to observe job writes
- mount a workspace at a known runtime path such as `/run/jobs/J/root`

## Orchestration Example

This is the shape of a job runner that wants a prepared filesystem tree, runs
another tool against it, waits for the job, and then removes the workspace:

```sh
sudo install -d -m 0755 /run/jobs/J

sudo ./overlayd --root /var/lib/overlayd init
sudo ./overlayd --root /var/lib/overlayd layer import ./base.tar base

WS_ROOT="$(
  sudo ./overlayd --root /var/lib/overlayd \
    ws create J -l base -m /run/jobs/J/root
)"

test "$WS_ROOT" = /run/jobs/J/root

sandbox --prepare-only "$WS_ROOT"

cat > /tmp/landlockd-runtime.toml <<EOF
[runtime]
root = "$WS_ROOT"
EOF

landlockd run --config /tmp/landlockd-runtime.toml -- /usr/bin/myapp
cgroupd wait J

sudo ./overlayd --root /var/lib/overlayd ws unmount J
sudo ./overlayd --root /var/lib/overlayd ws rm J
```

An executable version lives in
[docs/integration-example.sh](docs/integration-example.sh).

## Command Overview

```text
overlayd [--root PATH] [-v] <command> [args]

commands:
  init [path]
  layer create|list|rm|path|info|commit|import|export
  ws create|list|info|path|upper|mount|unmount|rm
  materialize <name> -l L1 [-l L2 ...] -m PATH
  version
  help
```

Layer order in `ws create` is top-to-bottom: the first `-l` is the uppermost
lower layer in the overlayfs stack.

## Privileges

Some operations only manipulate files in the store and only need normal write
access:

- `init`
- `layer create`
- `layer import`
- `layer export`
- `ws create --no-mount`

Operations that create or remove overlayfs mounts need mount privilege in the
current namespace:

- `ws create` without `--no-mount`
- `ws mount`
- `ws unmount`
- `materialize`

That can mean real root, `CAP_SYS_ADMIN`, or a mount-capable user namespace such
as `unshare -Urm`, depending on the host kernel and distribution settings.

See [docs/privileges.md](docs/privileges.md) for the detailed matrix and
rootless caveats.

## Current Status

`overlayd` is an early, focused implementation. The core layer and workspace
workflow is present, tests cover the CLI contracts and tar handling, and the
project is still intentionally small enough to audit.

The next useful improvements are likely around richer integration examples,
more production hardening, and clearer behavior around host-specific overlayfs
edge cases.
