# Privileges

## Kernel requirements

- Linux with `CONFIG_OVERLAY_FS=y` or `CONFIG_OVERLAY_FS=m`
- Kernel `>= 4.0` for basic overlayfs support
- Kernel `>= 4.19` if you intend to rely on features such as `redirect_dir` or `metacopy`

## Privilege options

| Execution mode | `init` / `layer *` | `ws create --no-mount` | `ws mount` / `ws unmount` / `materialize` / mounted `ws create` |
| --- | --- | --- | --- |
| real root | yes | yes | yes |
| `CAP_SYS_ADMIN` | yes | yes | yes |
| `unshare -Urm` | yes | yes | yes, if the kernel allows overlayfs in that user namespace |

`init` and `layer/*` only need write access to the store. Overlay mount lifecycle operations need mount privilege in the current namespace.

## Rootless limitations

User-namespace mounts are private to that namespace. If you run `overlayd` rootlessly with `unshare -Urm`, the orchestrator must also run `sandbox`, `landlockd`, and any later cleanup steps inside the same user and mount namespace. A mount created in that namespace is not usable by processes outside it.

## Overlayfs caveats for downstream daemons

- Rename and link behavior is subject to overlayfs semantics, not plain ext4/xfs semantics.
- First write to a lower-layer file triggers copy-up into the upperdir.
- Direct I/O against lower files is not available through overlayfs.
- Registered-file and advanced async I/O behavior may differ from a non-overlay filesystem.
