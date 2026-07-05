# L10GL - Lightweight Legacy OpenGL Driver Framework
#
# Backend structure:
#   src/l10gl.c          - Frontend dispatch layer
#   src/backends/mga1064 - Matrox MGA-1064SG (Mystique) backend
#   demos/cube.c         - Gouraud cube demo

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g -D_GNU_SOURCE -Isrc
LDFLAGS  = -lm

# Backend selection (which hardware driver to compile in)
BACKEND  = mga1064
BACKEND_SRCS = src/backends/$(BACKEND)/mga1064.c src/backends/$(BACKEND)/l10gl_mga1064.c

# Core sources (frontend + backend)
CORE_SRCS = src/l10gl.c $(BACKEND_SRCS)

# Demos
DEMOS = cube

.PHONY: all clean

all: $(DEMOS)

# Pattern: build demo from demos/X.c + core sources
cube: demos/cube.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/mga1064.h
	$(CC) $(CFLAGS) -o $@ demos/cube.c $(CORE_SRCS) $(LDFLAGS)

clean:
	rm -f $(DEMOS) *.o src/*.o src/backends/*/*.o
