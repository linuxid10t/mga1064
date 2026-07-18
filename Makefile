# L10GL - Lightweight Legacy OpenGL Driver Framework
#
# Backend structure:
#   src/l10gl.c                 - Frontend dispatch layer
#   src/backends/mga1064        - Matrox MGA-1064SG (Mystique) backend
#   src/backends/virge          - S3 ViRGE integrated 3D accelerator backend
#   demos/cube.c                - Gouraud cube demo
#   demos/textured_cube.c       - Textured cube demo (needs texture-capable backend)
#
# Usage:
#   make                      - Build every demo and diagnostic (default backend mga1064)
#   make BACKEND=virge        - Same, but select the S3 ViRGE backend for the demos
#   make <name>               - Build just one target (e.g. cube, cubefb, seamtest)

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g -D_GNU_SOURCE -Isrc
LDFLAGS  = -lm

# Backend selection (which hardware driver to compile in)
BACKEND  ?= virge

# Map backend name to source files
BACKEND_SRCS = src/backends/$(BACKEND)/$(BACKEND).c \
               src/backends/$(BACKEND)/l10gl_$(BACKEND).c

# Core sources (frontend + selected backend)
CORE_SRCS = src/l10gl.c src/pci_scan.c $(BACKEND_SRCS)

# Backend selection define for demos
ifeq ($(BACKEND),virge)
BACKEND_DEFINE = -DBACKEND_VIRGE
else
BACKEND_DEFINE = -DBACKEND_MGA1064
endif

# Interactive demos. cube/textured_cube/triangle/cubediag use the front-end
# API (built against the selected BACKEND); fbtest is backend-free (pure fbdev).
DEMOS = cube textured_cube triangle cubediag fbtest

# Diagnostics: virge-direct chip probes (link virge.c explicitly, independent
# of BACKEND) that drive the hardware and CPU-read VRAM back. Built by default.
TESTS = scantest filltest tritest gltritest fliptest dztest seamtest cubefb diagap texprobe

.PHONY: all clean

all: $(DEMOS) $(TESTS)

# Pattern: build demo from demos/X.c + core sources
cube: demos/cube.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/cube.c $(CORE_SRCS) $(LDFLAGS)

textured_cube: demos/textured_cube.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/textured_cube.c $(CORE_SRCS) $(LDFLAGS)

# Diagnostic: face-on UV-gradient textured quad + framebuffer readback, to
# verify the textured-triangle path's UV scaling/perspective (the one demo
# path never silicon-verified). Uses the frontend API (real bind/upload).
texprobe: demos/texprobe.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/texprobe.c $(CORE_SRCS) $(LDFLAGS)

triangle: demos/triangle.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/triangle.c $(CORE_SRCS) $(LDFLAGS)

# Diagnostic: rotating cube + per-face color legend, to inspect bleedthrough.
# Same render path as cube (front-end API), but flat full-saturation colors and
# a static color-key legend on the right. Optional arg freezes an orientation.
cubediag: demos/cubediag.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/cubediag.c $(CORE_SRCS) $(LDFLAGS)

# Diagnostic: CPU-drawn fbdev test pattern, no backend/engine involved.
# If this displays garbled, the mode is wrong and no engine fix will help.
fbtest: demos/fbtest.c
	$(CC) $(CFLAGS) -o $@ demos/fbtest.c

# Diagnostic: ViRGE scanout layout probe + CR67/pitch takeover experiment.
# Always builds against the virge backend (it is virge-specific), needs no
# frontend — just the chip driver.
scantest: demos/scantest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/scantest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: 2D engine fill readback test (symptom 2). Virge-specific,
# links only the chip driver (no frontend); CPU-reads VRAM after fills.
filltest: demos/filltest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/filltest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: 3D triangle readback test (symptom 2, 3D cutoff). Virge-specific,
# links only the chip driver (no frontend); CPU-reads VRAM after a 3D draw.
tritest: demos/tritest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/tritest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: 3D triangle readback through the DEMO engine sequence (symptom 2,
# 3D cutoff). Reproduces clear_z->draw (tritest) vs clear_z->fill->draw (demo)
# and tests sleep / FIFO-wait / re-arm interventions. Virge-specific, links
# only the chip driver (no frontend); CPU-reads VRAM after each draw.
gltritest: demos/gltritest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/gltritest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: CRTC page-flip probe (double-buffering groundwork). CPU-draws
# two patterns and cycles the display-start address through candidate byte
# divisors to settle the register unit on silicon, plus reports a working
# vsync detector. Virge-specific, links only the chip driver (no frontend).
fliptest: demos/fliptest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/fliptest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: measure the 3D engine's per-pixel X Z-gradient (TdZdX) on silicon
# (back-face bleedthrough). Draws a z=f(X) ramp triangle, Z-update ON, and
# CPU-reads the Z buffer to compute the rendered slope vs intended. Virge-
# specific, links only the chip driver (no frontend).
dztest: demos/dztest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/dztest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: measure the S3d triangle span-END fill rule (inclusive vs
# exclusive, ceil vs floor, both L/R directions) -- settles whether shared
# edges double-draw, the cause of the cube's bleedthrough band.
seamtest: demos/seamtest.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/seamtest.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: render the cube to VRAM and CPU-read it back (bypassing the
# monitor) to test whether the bleedthrough is in the framebuffer or monitor-side.
cubefb: demos/cubefb.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/cubefb.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

# Diagnostic: reproduce the cube's Left-face shared-diagonal notch in
# isolation (A-alone / B-alone / both Z=LESS x2 orders / both Z=ALWAYS) to
# split coverage-regime vs Z/draw-order. Virge-specific, links only the
# chip driver (no frontend); CPU-reads VRAM after each pass.
diagap: demos/diagap.c src/backends/virge/virge.c src/backends/virge/virge.h
	$(CC) $(CFLAGS) -o $@ demos/diagap.c src/backends/virge/virge.c src/pci_scan.c $(LDFLAGS)

clean:
	rm -f $(DEMOS) $(TESTS) *.o src/*.o src/backends/*/*.o
