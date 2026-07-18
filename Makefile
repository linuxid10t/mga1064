# L10GL - Lightweight Legacy OpenGL Driver Framework
#
# `make` builds the frontend, every hardware backend, all demos, and the
# retained ViRGE diagnostics. Frontend demos choose a backend at runtime;
# set L10GL_BACKEND=virge, mga1064, or swrast when running to force one.

CC       = gcc
AR       = ar
CFLAGS   = -Wall -Wextra -O2 -g -D_GNU_SOURCE -Isrc -MMD -MP
LDFLAGS  = -lm

LIBRARY = libl10gl.a
LIB_SRCS = \
	src/l10gl.c \
	src/l10gl_xform.c \
	src/pci_scan.c \
	src/backends/swrast/swrast.c \
	src/backends/virge/virge.c \
	src/backends/virge/l10gl_virge.c \
	src/backends/mga1064/mga1064.c \
	src/backends/mga1064/l10gl_mga1064.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

FRONTEND_DEMOS = cube textured_cube triangle cubediag
DEMOS = $(FRONTEND_DEMOS) fbtest

# All diagnostics except fbtest are ViRGE-specific. They still link against
# the complete library; archive extraction pulls only the objects they use.
TESTS = scantest filltest tritest gltritest fliptest dztest seamtest \
	cubefb diagap texprobe
CHECK_PROGRAMS = test-swrast test-xform

PROGRAMS = $(DEMOS) $(TESTS)
PROGRAM_OBJS = $(addprefix demos/,$(addsuffix .o,$(PROGRAMS)))
CHECK_OBJS = tests/test_swrast.o tests/test_xform.o
ALL_OBJS = $(LIB_OBJS) $(PROGRAM_OBJS) $(CHECK_OBJS)
DEPS = $(ALL_OBJS:.o=.d)

.PHONY: all check clean

all: $(LIBRARY) $(PROGRAMS)

check: all $(CHECK_PROGRAMS)
	bash tests/test-l10gl-run.sh
	./test-swrast
	./test-xform

$(LIBRARY): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(FRONTEND_DEMOS) $(TESTS): %: demos/%.o $(LIBRARY)
	$(CC) -o $@ $< $(LIBRARY) $(LDFLAGS)

test-swrast: tests/test_swrast.o $(LIBRARY)
	$(CC) -o $@ $< $(LIBRARY) $(LDFLAGS)

test-xform: tests/test_xform.o $(LIBRARY)
	$(CC) -o $@ $< $(LIBRARY) $(LDFLAGS)

# CPU-drawn fbdev pattern; deliberately independent of L10GL and PCI access.
fbtest: demos/fbtest.o
	$(CC) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(PROGRAMS) $(CHECK_PROGRAMS) $(LIBRARY) $(ALL_OBJS) $(DEPS)

-include $(DEPS)
