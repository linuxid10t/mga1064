# Adding an L10GL backend

This guide describes the backend contract as it exists today. Start by reading
`src/l10gl.h`, then use `src/backends/virge/` for a feature-rich example or
`src/backends/mga1064/` for the smaller baseline.

## Layering

Keep three responsibilities separate:

1. The frontend (`src/l10gl.c`) caches API state, selects a backend, and calls
   its vtable.
2. Backend glue (`src/backends/<chip>/l10gl_<chip>.c`) owns private state,
   converts `l10gl_vertex` values, and maps generic state to hardware bits.
3. The low-level driver (`src/backends/<chip>/<chip>.c`) discovers/maps the
   device and programs registers.

Chip-specific types, registers, and workarounds must not leak into `l10gl.c` or
applications. Keep direct diagnostic tools chip-specific instead of weakening
the frontend abstraction to accommodate them.

## Implement the lifecycle first

Add a `const struct l10gl_backend` vtable with a stable lowercase `name`.

- `probe()` is read-only and returns greater than zero when supported hardware
  is present, zero otherwise. It must not map BARs, change VGA state, request
  I/O permissions, or print a normal “device not found” error.
- `init()` allocates backend-private state, stores it in `ctx->backend_data`,
  maps resources, initializes the engine, and returns a negative errno-style
  value on failure.
- `cleanup()` waits for outstanding work, restores owned display state, unmaps
  resources, closes descriptors, and frees private state.

Use `l10gl_pci_find()` from `src/pci_scan.h` for sysfs discovery. Put all
supported device IDs in one backend-owned array and share one finder between
`probe()` and `init()`. Do not add another PCI directory scanner.

The `width`, `height`, and `bpp` arguments are a request. `bpp` is bytes per
pixel, not bits. If the backend adopts a different live raster, write the actual
geometry back to `l10gl_ctx` before returning. All later projection and buffer
layout code relies on those values being truthful.

## Fill the vtable incrementally

The minimum useful backend provides:

- lifecycle: `probe`, `init`, `cleanup`
- clearing: `clear_color`, and `clear_depth` when depth is advertised
- drawing: `draw_triangle`
- synchronization: `wait_engine`

Unsupported optional operations remain `NULL`. Set capability bits only for
paths the backend actually implements. The frontend currently falls back from
a textured triangle to a plain triangle when no textured draw hook exists; it
does not provide a general software rasterization fallback.

State setters should cache hardware-ready command bits in backend-private state
and apply them at draw time. The ViRGE glue demonstrates depth-test, depth-mask,
depth-function, blending, texture format, filter, and wrap mappings.

Vertices reaching a backend are already in screen coordinates:

- `x`, `y`: pixels
- `z`: normalized depth, near 0 and far 1
- `w`: reciprocal eye-space depth for perspective correction
- `r`, `g`, `b`, `a`: normalized channels
- `u`, `v`: normalized texture coordinates

The current hardware drivers sort triangle vertices and perform their own
fixed-point setup. Document every conversion next to the conversion helper;
fixed-point scale mistakes have historically produced plausible but badly
wrong hardware output.

## Register-programming discipline

Read the relevant primary datasheet section before adding a register write.
The repository indexes its sources in `docs/datasheets/README.md`. Cite the
document section and PDF page in code comments. If the datasheet is silent and
an external driver or emulator establishes behavior, label that source and the
remaining hardware-verification requirement.

Preserve initialization diagnostics until the path is proven on silicon. Never
use an unbounded hardware poll. Do not refactor a verified enable sequence in
the same commit as a behavioral change.

The ViRGE history illustrates the common bring-up traps:

- a register bank may have several independent enable gates;
- linear CPU addressing and engine addressing may be controlled separately;
- a command can require an explicit draw-enable bit;
- 2D commands may invalidate 3D state on real silicon;
- readable-looking MMIO registers may not reliably read back;
- scanout format, engine destination format, pitch, and buffer layout must all
  agree before visible output says anything about rasterization.

## Register and build the backend

After the backend builds independently:

1. Declare its vtable in `src/l10gl.h`.
2. Add it to `l10gl_backends[]` in `src/l10gl.c`. Order is selection priority;
   keep ViRGE first unless project policy changes.
3. Add its low-level and glue sources to `LIB_SRCS` in the Makefile.
4. Confirm `L10GL_BACKEND=<name>` forces it and an unknown name lists it.
5. Update the capability/status table in the README.

Run `make clean && make`. The build must compile every backend into
`libl10gl.a` with no warnings and build every demo and retained diagnostic.
On a development machine without supported hardware, also verify that automatic
detection exits cleanly and a forced backend reaches its own “device not found”
path.

## Hardware handoff

Hardware changes are tested on a separate machine that pulls `origin/main`.
Follow `docs/HANDOFF.md`: one logical change per commit, include the expected
hardware observation in the commit message, and push immediately after the
local compile gate. Record the exact card, PCI ID, command, logs, and visual or
readback result after the human test.
