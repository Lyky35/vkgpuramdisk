# NOTE
Current disk mount driver implementations create massive overhead.
!Won't produce great performance equal to your GPU bandwidth!
At best you can expect something between 5-15GB/s depending on your hardware.

# GpuRamDriveVk

GpuRamDriveVk is a Vulkan-backed RAM disk for Windows. It allocates a flat
block of GPU device memory and presents it to Windows through
[WinSpd](https://github.com/winfsp/winspd), a signed Storport virtual miniport
with a user-mode storage backend. Drives appear as normal volumes and can be
formatted, mounted, unmounted, saved to raw images, and mounted again.

This is a clean-room rewrite of GpuRamDrive with a C++20 code base, a Win32
GUI, a command-line front end, and a dynamically loaded Vulkan backend. WinSpd
and the Vulkan loader are resolved at run time.

## Requirements

- Windows 10 or later, x64.
- [WinSpd 1.0B1](https://github.com/winfsp/winspd/releases/tag/v1.0B1),
  including the WinSpd driver. GpuRamDriveVk does not bundle the driver.
- A Vulkan-capable GPU driver. The Vulkan loader (`vulkan-1.dll`) must be
  installed by the graphics driver.
- Administrator rights. The GUI manifest requests elevation; the CLI checks
  for elevation before privileged operations.

WinSpd is licensed under GPLv3 with a special exception for Free/Libre and
Open Source Software. See [Third-party notices](#third-party-notices).

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
- `build/gpuvramdisk-bench.exe` — raw Vulkan and legacy shared-memory proxy
  benchmark. See `docs/PERFORMANCE.md`.

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
and `--autostart`. The `--type` and `--removable` options are retained for
configuration compatibility; WinSpd always exposes a fixed SCSI disk.
`--read-only` requires an existing image.

`mount R` and `autostart` are foreground processes: the WinSpd storage unit
must stay alive while the volume is mounted. Press Ctrl+C, or run
`unmount R` from another elevated terminal, to save the image (if configured)
and unmount. If the image save fails, a non-forced unmount leaves the drive
mounted so the data is not lost. Stopping the WinSpd unit itself removes the
volume; close open files first.

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

A drive with an `image` path restores that raw volume image on mount and saves
it on a normal unmount. Images contain the filesystem payload, not the MBR;
the image is restored at the 1 MiB partition offset. Image saves are written to
`<image>.tmp` and atomically replaced, so a failed save does not destroy the
previous image. Relative image paths are resolved against the executable
directory.

A read-only drive requires an existing image and is mounted with WinSpd's
write-protect flag. The storage callback also rejects writes. A blank
read-only volume is not supported.

The `temp` option creates `<letter>:\Temp`, points `HKCU\Environment\TEMP` and
`TMP` at it, and restores the previous values on unmount. Only one mounted
drive may provide TEMP. A stale TEMP backup left by a crash is restored on the
next start when its drive letter is not in use.

Drive `C:` is reserved and is never accepted.

## Architecture

- `src/spd/` — minimal WinSpd ABI, dynamic DLL loading, and export resolution.
- `src/core/` — configuration, logging, WinSpd storage lifecycle, image I/O,
  TEMP redirection, Task Scheduler integration, and drive lifecycle.
- `src/gpu/` — dynamically resolved Vulkan API, device enumeration, and
  device-memory storage with concurrent I/O contexts.
- `src/gui/` — Win32 GUI and notification-area integration.
- `src/cli/` — command-line front end.
- `tools/` — benchmark harness and its benchmark-only legacy proxy.
- `resources/` — application icons.
- `third_party/Vulkan-Headers/` — Vulkan headers used only for compilation.

The logon task launches `gpuvramdrive.exe --autostart` when the GUI is next to
the registering executable, otherwise it falls back to the CLI.

### Storage path

```text
filesystem IRP
  -> WinSpd Storport miniport
  -> user-mode dispatcher threads (8 by default)
  -> Vulkan copy/submit/fence wait, or a CPU copy on the direct-mapped path
  -> GPU memory
```

`SpdStorage` writes an MBR with a 1 MiB partition start and an NTFS/exFAT or
FAT32 partition type, creates the WinSpd unit, starts its dispatcher, and waits
for the disk to appear. New volumes are
formatted and assigned with `diskpart`; restored images are assigned without
formatting. WinSpd is configured with a 4 MiB maximum transfer, no UNMAP, no
write cache, and a fixed product ID (`GpuRamDrive-<letter>`) so the correct
physical disk can be found.

On GPUs where device-local memory is host-visible, the drive is mapped
directly. Otherwise it uses device-local segments and a host-visible staging
buffer per transfer. Direct-mapped discrete GPUs keep direct writes but stage
reads through the GPU copy engine, because uncached BAR reads can be extremely
slow. Set `GPURAMDRIVE_FORCE_STAGING=1` to force the staging path for testing,
or use `GPURAMDRIVE_DIRECT_READS=1` / `GPURAMDRIVE_STAGED_READS=1` to override
the read policy. `GPURAMDRIVE_IO_CONTEXTS` controls the Vulkan I/O context pool
(default 8, clamped to 1–32).

The legacy shared-memory proxy remains compiled only for
`gpuvramdisk-bench`; the application no longer uses it.

## Verification

The following can be run under Wine with a software Vulkan driver (for
example Mesa lavapipe):

```sh
WINEPREFIX="$HOME/.wine-grd-test" wine build/gpuvramdrive-cli.exe gpus
WINEPREFIX="$HOME/.wine-grd-test" wine build/gpuvramdrive-cli.exe status
WINEPREFIX="$HOME/.wine-grd-test" wine build/gpuvramdisk-bench.exe --raw-only
```

Wine is only a functional smoke test. A WinSpd mount, `diskpart` format,
drive-letter assignment, image restore, TEMP, and Task Scheduler behavior
require an elevated real Windows host with the WinSpd driver installed.

## Limitations

- WinSpd 1.0B1 is a beta release. Current Windows 11 compatibility and driver
  signing are not guaranteed.
- GPU memory is not a replacement for system RAM; a GPU reset or driver
  timeout can lose the entire drive.
- The volume depends on the GUI or CLI process staying alive. If the process is
  killed, the WinSpd unit can be removed with unflushed data; unmount or reboot
  before reusing the letter.
- There is no journaling, encryption, or crash recovery beyond the optional
  raw image.
- Read-only is enforced by the WinSpd unit and the storage callback, but it is
  not an encryption or access-control boundary.
- The code is intended for Windows 10+ x64. The checked-in manifest is amd64.

## Third-party notices

WinSpd - Windows Storage Proxy Driver, Copyright (C) Bill Zissimopoulos

<https://github.com/winfsp/winspd>

WinSpd is licensed under GPLv3 with a special exception for Free/Libre and
Open Source Software. See the
[WinSpd license](https://github.com/winfsp/winspd/blob/master/License.txt) and
`THIRD_PARTY_NOTICES.md`.

## License

GpuRamDriveVk is MIT licensed. See the repository `LICENSE`.
