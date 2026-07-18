# L10GL — Lightweight Legacy OpenGL Driver Framework

L10GL is a userspace OpenGL-style driver framework for vintage fixed-function
graphics hardware. Applications call a small hardware-independent API; L10GL
detects a supported PCI card and dispatches directly to its MMIO drawing
engine.

There is no DRM, DRI, Mesa, kernel module, X11, or GLX. The current target is a
full-screen Linux console, programmed much like graphics hardware was in the
1990s.

## Current status

The S3 ViRGE is the primary backend and is tested on a real ViRGE/DX with 4 MB
of VRAM. The following paths are verified on silicon:

- Gouraud-shaded, depth-tested triangles
- perspective-correct texture mapping with repeat wrapping
- nearest and bilinear texture filtering
- depth-test, depth-mask, and depth-function state plumbing
- 2D rectangle fills
- native RGB555 scanout takeover when no fbdev driver owns the card
- double-buffered, vsync-synchronized page flips

The spinning `cube` and `textured_cube` demos render correctly and tear-free on
that machine. The Matrox MGA-1064SG backend builds and remains structurally
supported, but has not yet been validated on hardware.

| Backend | Hardware | Status |
|---|---|---|
| `virge` | S3 ViRGE family | Primary; ViRGE/DX verified on silicon |
| `mga1064` | Matrox Mystique/MGA-1064SG | Builds; hardware-unverified |

The detailed hardware history and test evidence live in
[`docs/HANDOFF.md`](docs/HANDOFF.md). The implementation roadmap is
[`PLAN.md`](PLAN.md).

## Architecture

```text
Application / demo
        │
        ▼
L10GL frontend (state cache + runtime backend registry)
        │
        ├── S3 ViRGE glue ────── ViRGE register driver
        └── MGA-1064 glue ────── MGA-1064 register driver
```

Each backend implements `struct l10gl_backend` from `src/l10gl.h`. The frontend
owns common render state and dispatches through that vtable. Backend glue
converts generic vertices and state into the low-level chip driver's formats.

PCI discovery is shared in `src/pci_scan.c`. Detection is read-only: ViRGE is
tried first, followed by MGA-1064. Initialization and hardware access only
begin after a backend has been selected.

## Requirements

- Linux on x86 hardware with permission to use legacy VGA I/O ports
- a supported PCI graphics card
- root privileges for PCI resource mappings and I/O-port access
- GCC, GNU Make, and standard Linux development headers

The ViRGE test machine intentionally has no `/dev/fb0`. In that configuration,
the backend adopts the live CRTC raster and changes scanout to the RGB555 format
required by the ViRGE 3D engine. A conventional fbdev console is still expected
by parts of the MGA path.

## Temporarily disable the kernel framebuffer

Use the reversible launcher when a kernel framebuffer or DRM fbdev-emulation
driver owns the card:

```sh
sudo tools/l10gl-run -- ./cube
```

The launcher selects the same card as L10GL, detaches every bound framebuffer
console (`fbcon`), unbinds the driver that owns `/dev/fb0`, and unbinds the
selected PCI function if it has a different driver. After the program exits it
rebinds the exact drivers in reverse order and then reattaches `fbcon`.

Inspect the complete plan without changing kernel state:

```sh
sudo tools/l10gl-run --dry-run -- ./cube
```

Backend and card overrides are supported:

```sh
sudo env L10GL_BACKEND=virge tools/l10gl-run \
    --device 0000:01:00.0 -- ./cube
```

Run this from SSH when possible: detaching `fbcon` can blank the local console
until L10GL takes over. Normal exits and signals restore ownership, but
`SIGKILL`, a launcher crash, or a system crash cannot run cleanup. Do not use
the launcher on a GPU serving an active graphical desktop.

The detach/reattach sequence follows the Linux kernel's
[`fbcon` documentation](https://docs.kernel.org/fb/fbcon.html) and the PCI
driver [`bind`/`unbind` sysfs ABI](https://docs.kernel.org/admin-guide/abi-testing-files.html#abi-sys-bus-pci-drivers-unbind).

## Build and run

Build the static library, all backends, demos, and retained diagnostics:

```sh
make
```

Run a demo as root:

```sh
sudo ./cube
sudo ./textured_cube
sudo ./cube 800 600 16
```

Frontend demos select hardware at runtime. Force a particular backend when
testing discovery or a multi-card machine:

```sh
sudo env L10GL_BACKEND=virge ./cube
sudo env L10GL_BACKEND=mga1064 ./cube
```

An unknown override is rejected and prints the available backend names. If no
supported card is present, demos fail without attempting MMIO access.

`make` produces `libl10gl.a`. An application can include `src/l10gl.h`, link
the archive and `libm`, then create a context with `l10gl_create_auto()`.

## Diagnostics

The repository retains the small ViRGE probes used to settle hardware behavior:
`scantest`, `filltest`, `tritest`, `gltritest`, `fliptest`, `dztest`, `seamtest`,
`cubefb`, `diagap`, and `texprobe`. They build with the normal `make` invocation
and intentionally bypass some frontend abstractions.

Use them only for the investigation described in their source comments and in
`docs/HANDOFF.md`; several directly manipulate scanout or inspect VRAM.

## Important limitation on exit

On the no-fbdev ViRGE/DX test machine, Ctrl-C restores the original CRTC mode
and scanout address, but it does not restore the original console pixels. The
BAR0 CPU mapping is write-combined and reads as zero, so the existing CPU
snapshot cannot capture the console. The parked fix is an engine-assisted
BitBLT to temporary VRAM (or a system-memory transfer path). Until then, the
last rendered frame remains visible in the restored console mode.

## Project layout

```text
src/
├── l10gl.c, l10gl.h             frontend API and backend registry
├── pci_scan.c, pci_scan.h       shared PCI sysfs discovery
└── backends/
    ├── virge/                   S3 ViRGE glue and register driver
    └── mga1064/                 Matrox Mystique glue and register driver
demos/                           demos and hardware diagnostics
tools/l10gl-run                  reversible fbcon/driver handoff launcher
docs/datasheets/                 primary hardware documentation
docs/HANDOFF.md                  silicon results and active handoff
PLAN.md                          phased implementation roadmap
```

See [`docs/BACKEND.md`](docs/BACKEND.md) before adding another card.

## License

MIT
