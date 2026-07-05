# L10GL — Lightweight Legacy OpenGL Driver Framework

A framework for writing userspace OpenGL-style drivers for vintage
fixed-function graphics hardware. Each card gets a backend that implements
a common interface; applications link against L10GL and are hardware-agnostic.

No DRM, no DRI, no Mesa, no kernel module. Just direct PCI MMIO register
programming, the way graphics worked before programmable shaders.

## Architecture

```
┌─────────────────────────────────────────┐
│           Application / Demo             │
│                 (cube.c)                 │
├─────────────────────────────────────────┤
│            L10GL Frontend                │
│  (l10gl.c — state cache, dispatch)      │
│  clear / draw_triangle / depth_func ...  │
├─────────────┬───────────────────────────┤
│  mga1064    │   (future backends)       │
│  backend    │   virge, rage, voodoo ... │
│  Matrox     │                           │
│  Mystique   │                           │
└─────────────┴───────────────────────────┘
```

### Frontend (`src/l10gl.c`, `src/l10gl.h`)

The frontend provides a GL-like API: `l10gl_clear`, `l10gl_draw_triangle`,
`l10gl_depth_func`, `l10gl_clear`, etc. It caches render state (depth func,
blend mode, clear values) and dispatches to the active backend through a
function-pointer vtable (`struct l10gl_backend`). This mirrors the classic
Mesa `dd_function_table` pattern.

### Backends (`src/backends/<chip>/`)

Each backend implements the `l10gl_backend` vtable for a specific GPU:

- **`mga1064`** — Matrox Mystique (MGA-1064SG, 1996)
  - Gouraud-shaded, Z-tested triangle rasterization in hardware
  - Bresenham line drawing in hardware
  - Rectangle fill / Z clear
  - PCI discovery, MMIO mapping, framebuffer access

## What it does (today)

The Mystique backend can draw **Gouraud-shaded, Z-buffered triangles**
entirely in hardware. The cube demo renders a spinning cube with per-face
directional diffuse lighting, hardware depth testing, and per-vertex color
interpolation — no CPU rasterization.

## Supported Hardware

| Backend | Chip | Capabilities | Status |
|---------|------|-------------|--------|
| `mga1064` | Matrox MGA-1064SG (Mystique) | Gouraud, Z-buffer, Lines | Compiles, untested on HW |

Future backends: 3Dfx Voodoo, S3 ViRGE, ATi Rage, Riva 128, etc.

## Requirements

- A supported vintage GPU installed in a Linux machine
- Working fbdev console (`matroxfb` or `vesafb` must have set the video mode)
- Root access (for `/dev/fb0` mmap and PCI resource access)
- 32-bit x86 (i686+)

## Build & Run

```bash
make
sudo ./cube              # 640x480 @ 16bpp (default)
sudo ./cube 800 600 16   # custom resolution
```

Ctrl-C to exit.

## Project Structure

```
src/
├── l10gl.h                    # Public API + backend interface
├── l10gl.c                    # Frontend dispatch + state cache
└── backends/
    └── mga1064/
        ├── mga1064.h           # Hardware register definitions (from datasheet)
        ├── mga1064.c           # PCI discovery, MMIO, engine init, drawing
        └── l10gl_mga1064.c     # Backend vtable glue (l10gl → mga1064)
demos/
└── cube.c                      # Spinning Gouraud cube
```

## Adding a New Backend

1. Create `src/backends/<yourchip>/`
2. Implement the `l10gl_backend` vtable (see `l10gl.h` for the struct)
3. Provide hardware init, triangle/line/rect drawing, buffer clearing
4. Register your backend: `extern const struct l10gl_backend mychip_backend;`
5. Compile by setting `BACKEND=mychip` in the Makefile

The minimum implementation needs: `init`, `cleanup`, `clear_color`,
`draw_triangle`, `wait_engine`. Everything else is optional — if the
hardware can't do it, set the function pointer to NULL and the frontend
will skip it or provide a software fallback.

## License

MIT
