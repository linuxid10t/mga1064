/*
 * test_quake_linkgate.c — Phase 7 Q0 link gate.
 *
 * Purpose: fail loudly if libl10gl.a does not provide every GL core entry
 * point the id GLQuake source release calls directly (the manifest in
 * tests/quake_gl_symbols.def), and report token/header coverage. This is
 * the Quake-scoped analogue of the Phase 8 C0 coverage gate.
 *
 * Mechanism: each manifest symbol gets a weakref alias. Linked with
 * -Wl,--whole-archive, every archive member is pulled in, so a weakref
 * resolves to the real definition when libl10gl.a provides it and stays
 * NULL when it does not. The binary always links; it exits nonzero iff any
 * required function symbol is absent. (Without --whole-archive a weak
 * reference would not extract the archive member and every symbol would
 * falsely read as missing.)
 *
 * After Q0 this gate is intentionally RED and documents the missing symbols;
 * Q1 lands them and turns it green. Tokens are reported but do not fail the
 * gate, because Quake-referenced tokens land across Q1 (queries, hints,
 * fill), Q2 (GL_POLYGON), Q5 (GL_ALPHA_TEST), and Q6 (texture env).
 */

#include <stdio.h>

#include <GL/gl.h>

/* ---- function-symbol gate (strict) ------------------------------------- */

/* Weakref aliases: &wq_<name> is non-NULL iff libl10gl.a defines <name>. */
#define QGL_SYMBOL(name) \
	static void wq_##name(void) __attribute__((weakref(#name)));
#include "quake_gl_symbols.def"
#undef QGL_SYMBOL

struct qgl_sym {
	const char *name;
	void *addr;
};

/* Names table. */
#define QGL_SYMBOL(name) {#name, NULL},
static struct qgl_sym qgl_syms[] = {
#include "quake_gl_symbols.def"
};
#undef QGL_SYMBOL

static const int qgl_nsyms = (int)(sizeof(qgl_syms) / sizeof(qgl_syms[0]));

static void qgl_resolve(void)
{
	int i = 0;
#define QGL_SYMBOL(name) qgl_syms[i++].addr = (void *)&wq_##name;
#include "quake_gl_symbols.def"
#undef QGL_SYMBOL
}

/* ---- token / header coverage (informational) --------------------------- */

static int tok_present;
static int tok_total;
static int tok_first_missing_printed;

/* HAVE references the token's value so a defined-but-broken macro is caught;
 * MISS records a gap. Neither affects the exit code. */
#define HAVE_TOK(name)                                                     \
	do {                                                               \
		tok_total++;                                               \
		tok_present++;                                             \
		(void)(long)(name);                                        \
	} while (0)
#define MISS_TOK(name)                                                     \
	do {                                                               \
		tok_total++;                                               \
	} while (0)

static void check_tokens(void)
{
#ifdef GL_POLYGON
	HAVE_TOK(GL_POLYGON);
#else
	MISS_TOK(GL_POLYGON);
#endif
#ifdef GL_QUADS
	HAVE_TOK(GL_QUADS);
#else
	MISS_TOK(GL_QUADS);
#endif
#ifdef GL_TRIANGLES
	HAVE_TOK(GL_TRIANGLES);
#else
	MISS_TOK(GL_TRIANGLES);
#endif
#ifdef GL_TRIANGLE_FAN
	HAVE_TOK(GL_TRIANGLE_FAN);
#else
	MISS_TOK(GL_TRIANGLE_FAN);
#endif
#ifdef GL_TRIANGLE_STRIP
	HAVE_TOK(GL_TRIANGLE_STRIP);
#else
	MISS_TOK(GL_TRIANGLE_STRIP);
#endif
#ifdef GL_LINES
	HAVE_TOK(GL_LINES);
#else
	MISS_TOK(GL_LINES);
#endif
#ifdef GL_ALPHA_TEST
	HAVE_TOK(GL_ALPHA_TEST);
#else
	MISS_TOK(GL_ALPHA_TEST);
#endif
#ifdef GL_BLEND
	HAVE_TOK(GL_BLEND);
#else
	MISS_TOK(GL_BLEND);
#endif
#ifdef GL_CULL_FACE
	HAVE_TOK(GL_CULL_FACE);
#else
	MISS_TOK(GL_CULL_FACE);
#endif
#ifdef GL_DEPTH_TEST
	HAVE_TOK(GL_DEPTH_TEST);
#else
	MISS_TOK(GL_DEPTH_TEST);
#endif
#ifdef GL_TEXTURE_2D
	HAVE_TOK(GL_TEXTURE_2D);
#else
	MISS_TOK(GL_TEXTURE_2D);
#endif
#ifdef GL_COLOR_BUFFER_BIT
	HAVE_TOK(GL_COLOR_BUFFER_BIT);
#else
	MISS_TOK(GL_COLOR_BUFFER_BIT);
#endif
#ifdef GL_DEPTH_BUFFER_BIT
	HAVE_TOK(GL_DEPTH_BUFFER_BIT);
#else
	MISS_TOK(GL_DEPTH_BUFFER_BIT);
#endif
#ifdef GL_ZERO
	HAVE_TOK(GL_ZERO);
#else
	MISS_TOK(GL_ZERO);
#endif
#ifdef GL_ONE
	HAVE_TOK(GL_ONE);
#else
	MISS_TOK(GL_ONE);
#endif
#ifdef GL_SRC_ALPHA
	HAVE_TOK(GL_SRC_ALPHA);
#else
	MISS_TOK(GL_SRC_ALPHA);
#endif
#ifdef GL_ONE_MINUS_SRC_ALPHA
	HAVE_TOK(GL_ONE_MINUS_SRC_ALPHA);
#else
	MISS_TOK(GL_ONE_MINUS_SRC_ALPHA);
#endif
#ifdef GL_ONE_MINUS_SRC_COLOR
	HAVE_TOK(GL_ONE_MINUS_SRC_COLOR);
#else
	MISS_TOK(GL_ONE_MINUS_SRC_COLOR);
#endif
#ifdef GL_LEQUAL
	HAVE_TOK(GL_LEQUAL);
#else
	MISS_TOK(GL_LEQUAL);
#endif
#ifdef GL_GEQUAL
	HAVE_TOK(GL_GEQUAL);
#else
	MISS_TOK(GL_GEQUAL);
#endif
#ifdef GL_GREATER
	HAVE_TOK(GL_GREATER);
#else
	MISS_TOK(GL_GREATER);
#endif
#ifdef GL_FRONT
	HAVE_TOK(GL_FRONT);
#else
	MISS_TOK(GL_FRONT);
#endif
#ifdef GL_BACK
	HAVE_TOK(GL_BACK);
#else
	MISS_TOK(GL_BACK);
#endif
#ifdef GL_FRONT_AND_BACK
	HAVE_TOK(GL_FRONT_AND_BACK);
#else
	MISS_TOK(GL_FRONT_AND_BACK);
#endif
#ifdef GL_FLAT
	HAVE_TOK(GL_FLAT);
#else
	MISS_TOK(GL_FLAT);
#endif
#ifdef GL_SMOOTH
	HAVE_TOK(GL_SMOOTH);
#else
	MISS_TOK(GL_SMOOTH);
#endif
#ifdef GL_MODELVIEW
	HAVE_TOK(GL_MODELVIEW);
#else
	MISS_TOK(GL_MODELVIEW);
#endif
#ifdef GL_PROJECTION
	HAVE_TOK(GL_PROJECTION);
#else
	MISS_TOK(GL_PROJECTION);
#endif
#ifdef GL_MODELVIEW_MATRIX
	HAVE_TOK(GL_MODELVIEW_MATRIX);
#else
	MISS_TOK(GL_MODELVIEW_MATRIX);
#endif
#ifdef GL_TEXTURE_MAG_FILTER
	HAVE_TOK(GL_TEXTURE_MAG_FILTER);
#else
	MISS_TOK(GL_TEXTURE_MAG_FILTER);
#endif
#ifdef GL_TEXTURE_MIN_FILTER
	HAVE_TOK(GL_TEXTURE_MIN_FILTER);
#else
	MISS_TOK(GL_TEXTURE_MIN_FILTER);
#endif
#ifdef GL_TEXTURE_WRAP_S
	HAVE_TOK(GL_TEXTURE_WRAP_S);
#else
	MISS_TOK(GL_TEXTURE_WRAP_S);
#endif
#ifdef GL_TEXTURE_WRAP_T
	HAVE_TOK(GL_TEXTURE_WRAP_T);
#else
	MISS_TOK(GL_TEXTURE_WRAP_T);
#endif
#ifdef GL_NEAREST
	HAVE_TOK(GL_NEAREST);
#else
	MISS_TOK(GL_NEAREST);
#endif
#ifdef GL_LINEAR
	HAVE_TOK(GL_LINEAR);
#else
	MISS_TOK(GL_LINEAR);
#endif
#ifdef GL_NEAREST_MIPMAP_NEAREST
	HAVE_TOK(GL_NEAREST_MIPMAP_NEAREST);
#else
	MISS_TOK(GL_NEAREST_MIPMAP_NEAREST);
#endif
#ifdef GL_LINEAR_MIPMAP_NEAREST
	HAVE_TOK(GL_LINEAR_MIPMAP_NEAREST);
#else
	MISS_TOK(GL_LINEAR_MIPMAP_NEAREST);
#endif
#ifdef GL_NEAREST_MIPMAP_LINEAR
	HAVE_TOK(GL_NEAREST_MIPMAP_LINEAR);
#else
	MISS_TOK(GL_NEAREST_MIPMAP_LINEAR);
#endif
#ifdef GL_LINEAR_MIPMAP_LINEAR
	HAVE_TOK(GL_LINEAR_MIPMAP_LINEAR);
#else
	MISS_TOK(GL_LINEAR_MIPMAP_LINEAR);
#endif
#ifdef GL_REPEAT
	HAVE_TOK(GL_REPEAT);
#else
	MISS_TOK(GL_REPEAT);
#endif
#ifdef GL_TEXTURE_ENV
	HAVE_TOK(GL_TEXTURE_ENV);
#else
	MISS_TOK(GL_TEXTURE_ENV);
#endif
#ifdef GL_TEXTURE_ENV_MODE
	HAVE_TOK(GL_TEXTURE_ENV_MODE);
#else
	MISS_TOK(GL_TEXTURE_ENV_MODE);
#endif
#ifdef GL_MODULATE
	HAVE_TOK(GL_MODULATE);
#else
	MISS_TOK(GL_MODULATE);
#endif
#ifdef GL_REPLACE
	HAVE_TOK(GL_REPLACE);
#else
	MISS_TOK(GL_REPLACE);
#endif
#ifdef GL_RGB
	HAVE_TOK(GL_RGB);
#else
	MISS_TOK(GL_RGB);
#endif
#ifdef GL_RGBA
	HAVE_TOK(GL_RGBA);
#else
	MISS_TOK(GL_RGBA);
#endif
#ifdef GL_UNSIGNED_BYTE
	HAVE_TOK(GL_UNSIGNED_BYTE);
#else
	MISS_TOK(GL_UNSIGNED_BYTE);
#endif
#ifdef GL_FLOAT
	HAVE_TOK(GL_FLOAT);
#else
	MISS_TOK(GL_FLOAT);
#endif
#ifdef GL_LUMINANCE
	HAVE_TOK(GL_LUMINANCE);
#else
	MISS_TOK(GL_LUMINANCE);
#endif
#ifdef GL_ALPHA
	HAVE_TOK(GL_ALPHA);
#else
	MISS_TOK(GL_ALPHA);
#endif
#ifdef GL_INTENSITY
	HAVE_TOK(GL_INTENSITY);
#else
	MISS_TOK(GL_INTENSITY);
#endif
#ifdef GL_RGBA4
	HAVE_TOK(GL_RGBA4);
#else
	MISS_TOK(GL_RGBA4);
#endif
#ifdef GL_PERSPECTIVE_CORRECTION_HINT
	HAVE_TOK(GL_PERSPECTIVE_CORRECTION_HINT);
#else
	MISS_TOK(GL_PERSPECTIVE_CORRECTION_HINT);
#endif
#ifdef GL_FASTEST
	HAVE_TOK(GL_FASTEST);
#else
	MISS_TOK(GL_FASTEST);
#endif
#ifdef GL_NICEST
	HAVE_TOK(GL_NICEST);
#else
	MISS_TOK(GL_NICEST);
#endif
#ifdef GL_VENDOR
	HAVE_TOK(GL_VENDOR);
#else
	MISS_TOK(GL_VENDOR);
#endif
#ifdef GL_RENDERER
	HAVE_TOK(GL_RENDERER);
#else
	MISS_TOK(GL_RENDERER);
#endif
#ifdef GL_VERSION
	HAVE_TOK(GL_VERSION);
#else
	MISS_TOK(GL_VERSION);
#endif
#ifdef GL_EXTENSIONS
	HAVE_TOK(GL_EXTENSIONS);
#else
	MISS_TOK(GL_EXTENSIONS);
#endif
#ifdef GL_FILL
	HAVE_TOK(GL_FILL);
#else
	MISS_TOK(GL_FILL);
#endif
}

int main(void)
{
	qgl_resolve();

	int present = 0;
	int missing = 0;

	printf("quake link gate: %d required GL core symbols\n", qgl_nsyms);
	for (int i = 0; i < qgl_nsyms; i++) {
		if (qgl_syms[i].addr != NULL) {
			present++;
		} else {
			if (!missing)
				printf("  MISSING:\n");
			printf("    %s\n", qgl_syms[i].name);
			missing++;
		}
	}
	printf("  present: %d/%d, missing: %d\n", present, qgl_nsyms,
	       missing);

	(void)tok_first_missing_printed;
	check_tokens();
	printf("quake token coverage: %d/%d GLQuake-referenced tokens defined\n",
	       tok_present, tok_total);

	if (missing) {
		printf("FAIL: %d required symbol(s) absent; Q1 must land them.\n",
		       missing);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
