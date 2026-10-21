# GpuRamDriveVk

GpuRamDriveVk is a Vulkan-backed RAM disk for Windows. It allocates a flat
block of GPU device memory and exposes it to Windows through ImDisk's
shared-memory proxy driver. Drives appear as normal volumes and can be
formatted, mounted, unmounted, saved to raw images, and mounted again.

This is a clean-room rewrite of GpuRamDrive with a C++20 code base, a Win32
GUI, a command-line front end, and a Vulkan backend that does not require the
Vulkan SDK at build time or `vulkan-1.lib` at run time. Vulkan and ImDisk are
loaded dynamically.

## Requirements

- Windows 10 or later, x64.
- The [ImDisk Virtual Disk Driver](https://www.ltr-data.se/opencode.html),
  including `imdisk.cpl`.
- A Vulkan-capable GPU driver. The Vulkan loader (`vulkan-1.dll`) must be
  installed by the graphics driver.
- Administrator rights. The GUI manifest requests elevation; the CLI checks
  for elevation before privileged operations.

## Building

The checked-in build configuration cross-compiles from Linux with MinGW-w64:

```sh
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Outputs:

- `build/gpuvramdrive.exe` — GUI, `requireAdministrator`.
- `build/gpuvramdrive-cli.exe` — CLI, `asInvoker` plus explicit elevation checks.
- `build/gpuvramdisk-bench.exe` — raw Vulkan and shared-memory proxy benchmark.
  See `docs/PERFORMANCE.md` for the measurement method and current findings.

Dependencies: CMake 3.20+, a C++20 MinGW-w64 toolchain, and the Vulkan headers
under `third_party/Vulkan-Headers/include`. The executables do not import
`vulkan-1.dll` directly; the loader is opened at runtime.

## GUI

Run `gpuvramdrive.exe` as administrator. Select a drive row to edit its
configuration. The GUI can add/update/remove configurations, mount/unmount,
format, save images, toggle the logon task, and browse for an image path.

Closing the window hides it to the notification area. Use **Exit** from the
tray menu or File menu to unmount drives and quit. Mounted drives stay alive
while the GUI is running.

## CLI

Run `gpuvramdrive-cli.exe` from an elevated terminal.

```text
gpus
status
where
add R --size 1024 --fs exFAT --label GpuDisk --gpu 0
mount R
unmount R
save R C:\images\R.img
restore R C:\images\R.img
autostart-task on|off|status
```

`add` supports `--size` (MiB), `--gpu`, `--fs` (`NTFS`, `exFAT`, `FAT32`),
`--label`, `--image`, `--type hd|fd`, `--removable`, `--read-only`, `--temp`,
and `--autostart`.

`mount R` and `autostart` are foreground processes: the shared-memory proxy
must stay alive while the volume is mounted. Press Ctrl+C, or run
`unmount R` from another elevated terminal, to save the image (if configured)
and unmount. If a normal unmount is blocked by open files, the CLI does not
force it; unmount the drive from the GUI or close the files first.

`unmount` only signals a foreground CLI mount process. Drives mounted by the
GUI must be unmounted from the GUI. When an `autostart` process is handling
several drives, a stop request makes it save and unmount all of them.

## Configuration and data

The configuration file is:

```text
%LOCALAPPDATA%\GpuRamDriveVk\drives.ini
```

Each `[drive.X]` section stores the drive letter, GPU index, size, filesystem,
device type, label, image path, and flags. Logs are written to
`gpuvramdrive.log` in the same directory.

A drive with an `image` path restores that raw image on mount and saves it on
a normal unmount. Image saves are written to `<image>.tmp` and atomically
replaced, so a failed save does not destroy the previous image. Relative image
paths are resolved against the executable directory. A read-only drive with an image path refuses to mount when the
image is missing. Read-only drives without an image start as a blank
formatted volume and discard changes on unmount.

The `temp` option creates `<letter>:\Temp`, points `HKCU\Environment\TEMP` and
`TMP` at it, and restores the previous values on unmount. Only one mounted
drive may provide TEMP. A stale TEMP backup left by a crash is restored on the
next start when its drive letter is not in use.

Drive `C:` is reserved and is never accepted.

## Architecture

- `src/core/` — configuration, logging, ImDisk API binding, shared-memory
  proxy server, image I/O, TEMP redirection, Task Scheduler integration, and
  drive lifecycle.
- `src/gpu/` — dynamically resolved Vulkan API, device enumeration, and
  device-memory storage.
- `src/gui/` — Win32 GUI and notification-area integration.
- `src/cli/` — command-line front end.
- `resources/` — application icons.

The logon task launches `gpuvramdrive.exe --autostart` when the GUI is next to
the registering executable, otherwise it falls back to the CLI.
- `third_party/Vulkan-Headers/` — Vulkan headers used only for compilation.

On GPUs where device-local memory is host-visible, the drive is mapped
directly. Otherwise it uses 256 MiB device-local segments and a 4 MiB
host-visible staging buffer per transfer. Set
`GPURAMDRIVE_FORCE_STAGING=1` to force the staging path for testing.

The ImDisk wire protocol is the standard 4096-byte shared-memory header plus a
4 MiB data area. The proxy reports 512-byte request alignment because the
Vulkan staging path uses buffer copies and Windows storage I/O is
sector-aligned. Read, write, info, and close requests are supported; unknown
requests return `ENODEV`. UNMAP/ZERO are not advertised.

## Verification

The following can be run under Wine with a software Vulkan driver (for
example Mesa lavapipe):

```sh
WINEPREFIX="$HOME/.wine-grd-test" wine build/gpuvramdrive-cli.exe gpus
WINEPREFIX="$HOME/.wine-grd-test" wine build/gpuvramdrive-cli.exe status
```

The GPU allocation path, direct and forced-staging paths, and the proxy
protocol (info/write/read/close, alignment, bounds, and unknown-request
handling) have been exercised with a software Vulkan device. Full ImDisk mount, format,
image, TEMP, and Task Scheduler behavior requires a real elevated Windows
host with ImDisk installed.

## Limitations

- GPU memory is not a replacement for system RAM; a GPU reset or driver
  timeout can lose the entire drive.
- The volume depends on the GUI or CLI process staying alive. If the process
  is killed, the ImDisk volume can be left in a failed state; unmount or
  reboot before reusing the letter.
- There is no journaling, encryption, or crash recovery beyond the optional
  raw image.
- A read-only drive is created from a writable temporary mount and then
  re-mounted read-only; do not rely on this as a security boundary.
- The code is intended for Windows 10+ x64. The checked-in manifest is amd64.

## License

MIT. See the repository `LICENSE`.
