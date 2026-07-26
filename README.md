# Drive2Iso

Capture a live/installed **partition** and turn it into a **bootable ISO** — one
isohybrid image that boots when `dd`'d onto a hard drive *and* when flashed to a
USB stick, on both **BIOS and UEFI**. Ships as `drive2iso.exe` (Windows) and an
AppImage (Linux), over one shared C core.

Drive2Iso is the **inverse of [ISO2Drive](../ISO2Drive)**: ISO2Drive takes an ISO
→ puts it on a drive; Drive2Iso takes a drive → makes an ISO. It mirrors that
project's shape — a portable `src/core`, per-host backends behind a vtable, an
`$(OS)`-selected Makefile, dry-run-until-`--commit`, and the Etcher-style
three-step flow, now **SOURCE → CAPTURE → IMAGE**.

## What "live isohybrid ISO" means per OS (read this first)

The phrase is honest on Linux and aspirational on Windows, so the tool treats
them differently:

| | Linux | Windows |
|---|---|---|
| Payload | `filesystem.squashfs` (mksquashfs) | `install.wim` (wimlib/DISM) |
| Boot env | live-boot/dracut initrd + GRUB | **WinPE** (from the Windows ADK) |
| Result | a **live** system that runs off the ISO | **install/restore** media that re-images the WIM |
| Built with | `grub-mkrescue` / `xorriso` (isohybrid) | `oscdimg` / `xorriso` (El Torito) |

There is **no true live-Windows-from-ISO** outside the deprecated Windows-To-Go,
so the Windows artifact is WinPE + a captured WIM — bootable media that applies
the image to a target disk. The Linux path is real end-to-end; the Windows
**capture** (VSS snapshot + WIM) is real, and the **ISO assembly is gated on the
host Windows ADK** (WinPE + `oscdimg`) — the same way ISO2Drive gates its BIOS
GRUB path on bundled binaries. It never produces a silent partial.

## Layout

```
include/drive2iso/   public headers (backend vtable + core API)
src/core/            PORTABLE core — no platform code
  util.c  ui.c  flash.c   reused verbatim from ISO2Drive
  capture.c    exclude rules + ISO staging-tree layout
  bootcfg.c    per-distro live-boot profiles + grub.cfg generation
  isobuild.c   grub-mkrescue / xorriso / oscdimg command builder
src/backend/
  backend_linux.c    AppImage host: ro-mount + mksquashfs + live initrd + grub-mkrescue [REAL]
  backend_windows.c  Windows host: VSS + wimlib/DISM capture; ISO assembly gated on the ADK
  winvss.c           VSS snapshot via Win32_ShadowCopy (works on client editions)
  winutil.c winusb.c firmware/elevation probes + raw PhysicalDrive IO (from ISO2Drive)
src/main.c           thin CLI frontend
packaging/AppDir/    AppRun + .desktop + icon for appimagetool
```

## Build

MSYS2/MinGW (Windows) or any Linux — the Makefile auto-selects the host backend:

```bash
make
```

Produces `drive2iso.exe` (Windows) or `drive2iso` (Linux).

Bundle the Linux binary as an AppImage:

```bash
sh packaging/build-appimage.sh      # make + stage into AppDir + appimagetool
```

## Usage

```bash
drive2iso list-parts                          # list candidate source partitions
drive2iso probe /dev/sda2                      # fs / os / size / firmware / live?
drive2iso doctor                               # tool availability + firmware mode
drive2iso capture /dev/sda2 out.iso            # DRY-RUN: print the full plan
drive2iso capture /dev/sda2 out.iso --commit   # actually build the ISO
drive2iso write-usb out.iso /dev/sdb --verify  # raw dd-style flash of the result
```

**`capture` is a dry-run until `--commit`** — it prints the exact
`mksquashfs`/`wimlib`/`xorriso` (or `oscdimg`) commands and changes nothing.
`--commit` needs root (Linux) or an elevated prompt (Windows, for VSS + capture).

`capture` options: `--no-uefi` `--no-bios` `--no-isohybrid` `--comp <level>`
`--work <dir>` (staging/scratch dir; default: temp/drive2iso).

Device forms: Linux `/dev/sdXN` **or** a mountpoint; Windows a drive letter (`C:`).

### How the result boots

`grub-mkrescue` emits an isohybrid image whose GRUB handles **BIOS via El Torito
and UEFI** from the same file. Because it is isohybrid it boots equally when:

- flashed to a USB stick (`write-usb out.iso /dev/sdb --commit`), or
- `dd`'d onto a whole disk.

A live ISO written to a disk boots as a **live** (read-only squashfs + tmpfs
overlay) system — not a persistent install. Persistence and an in-ISO installer
are on the roadmap.

## Windows specifics

- Run from an **elevated** prompt: VSS snapshots and volume capture need admin.
- Snapshots use the **Win32_ShadowCopy** WMI class (present on every client
  edition), not `diskshadow.exe` (Server-only).
- The bootable-ISO step needs the **Windows ADK** + the **Windows PE add-on**
  (`oscdimg`, WinPE base). Run Drive2Iso from the *Deployment and Imaging Tools
  Environment* so `copype`/`oscdimg` are on `PATH`. Without it, `capture`
  produces the WIM + a full dry-run plan and tells you what to install.
- Capturing with `wimlib-imagex` is preferred; `DISM` is the fallback.
- To flash the result onto a FAT32 USB, split a >4 GiB WIM
  (`wimlib-imagex split` / `dism /Split-Image`).

## Verify end-to-end (Linux)

```bash
# 1) dry-run anywhere (safe): prints the mksquashfs + grub-mkrescue plan
drive2iso capture /dev/sda2 out.iso

# 2) real build of a small test rootfs on a loop device, then boot both ways:
drive2iso capture /dev/loop0p1 out.iso --commit
qemu-system-x86_64 -cdrom out.iso                 # USB/optical boot (BIOS)
qemu-system-x86_64 -bios OVMF.fd -cdrom out.iso    # UEFI boot
qemu-system-x86_64 -hda out.iso                    # dd-to-disk boot
```

## What's real vs gated

**Real:** partition probe + listing; the portable capture rules, staging-tree
layout, per-distro live profiles, and imager-command generation; **Linux capture
→ live isohybrid ISO** end-to-end (mksquashfs + grub-mkrescue, dry-run preview,
system-disk guards); **Windows VSS snapshot + WIM capture**; **raw `write-usb`**
on both hosts (progress bar, read-back verify, system-disk / non-removable
guards) — reused from ISO2Drive.

**Gated / roadmap:** Windows bootable-ISO assembly needs the ADK on `PATH`;
the fedora (dracut) and arch (mkinitcpio) live-initrd layouts are tabled but not
yet wired to their native tooling; a live initrd on Debian/Ubuntu requires the
`live-boot` package present in the source (the tool prints the regen command).

## Per-distro live logic

`src/core/bootcfg.c` holds the family table (kernel cmdline + squashfs layout)
in exactly one place — the debian/ubuntu/generic rows (`live-boot`,
`/live/filesystem.squashfs`) are the fully-wired v1 path. Validate it against
real systems; **penguins-eggs** is the authoritative reference for turning an
installed system into a redistributable live ISO.

## Roadmap

1. **Persistence** — a writable overlay so a dd'd/USB live session keeps state.
2. **In-ISO installer** — turn "boots live" into "can install to disk".
3. **Windows** — auto-detect/fetch ADK bits; an unattended WIM-apply script on
   the WinPE media; automatic >4 GiB WIM split for FAT targets.
4. Wire the fedora/arch live-initrd paths; validate the profile table.
