/*
 * cubefb.c - render the cube to VRAM and CPU-read it back, to settle whether
 * the face-color bleed lives in the framebuffer or is introduced later.
 *
 * Three contamination signals (the first version had only blends):
 *   - BLENDS:      pixels that are no pure face color and not background.
 *   - MISPLACED:   a face-color pixel sitting INSIDE another face's region --
 *                  4-connected, >=3 neighbors of a DIFFERENT face and no
 *                  background neighbor (a clean shared edge has only ~1
 *                  other-face neighbor, so it is not flagged). Direct detector
 *                  for "blue on teal": solid blue surrounded by teal.
 *   - EXCESS:      a visible face rendering more px than its projected area
 *                  (sum of |det|/2 over its two triangles) -- catches a thin
 *                  sliver spilling onto a neighbor.
 *   - HOLES:       a background pixel >1.5px INSIDE the projected cube
 *                  silhouette (= convex hull of the 8 projected vertices,
 *                  exact for a convex solid). Direct detector for coverage
 *                  gaps -- "black wedge growing out of a face corner" --
 *                  which the other signals are blind to (MISPLACED skips
 *                  any pixel with a background neighbor).
 *
 * SWEEPS 36 orientations over a full rotation (the reported bleed is
 * orientation-specific: Top near-parallel + Left near-edge-on), flags any
 * contaminated angle, and prints full detail + the middle-row readback for
 * the worst. The framebuffer is CPU-read directly, bypassing the monitor, so
 * any contamination here is a real per-frame VRAM artifact.
 *
 * Build: make -B BACKEND=virge cubefb     Run: sudo ./cubefb [N]
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

#define PI 3.14159265358979323846f

static void build_rotation(float m[3][3], float ax, float ay)
{
    float sx = sinf(ax), cx = cosf(ax);
    float sy = sinf(ay), cy = cosf(ay);
    m[0][0] = cy;     m[0][1] = 0;      m[0][2] = sy;
    m[1][0] = sx*sy;  m[1][1] = cx;     m[1][2] = -sx*cy;
    m[2][0] = -cx*sy; m[2][1] = sx;     m[2][2] = cx*cy;
}

static void mat3_transform(float out[3], const float m[3][3], const float in[3])
{
    out[0] = m[0][0]*in[0] + m[0][1]*in[1] + m[0][2]*in[2];
    out[1] = m[1][0]*in[0] + m[1][1]*in[1] + m[1][2]*in[2];
    out[2] = m[2][0]*in[0] + m[2][1]*in[1] + m[2][2]*in[2];
}

struct screen_vertex { float sx, sy, sz; };

static void project(struct screen_vertex *o, const float in[3], int sw, int sh, float cd)
{
    float z = in[2] + cd;
    if (z < 0.1f) z = 0.1f;
    float s = (float)sh / z;
    o->sx = (float)sw * 0.5f + in[0] * s;
    o->sy = (float)sh * 0.5f - in[1] * s;
    o->sz = (in[2] + cd - 3.0f) / 4.0f;
    if (o->sz < 0.0f) o->sz = 0.0f;
    if (o->sz > 1.0f) o->sz = 1.0f;
}

static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};
static const int cube_faces[12][3] = {
    {0, 2, 1}, {0, 3, 2},  {4, 5, 6}, {4, 6, 7},  {0, 4, 7}, {0, 7, 3},
    {1, 2, 6}, {1, 6, 5},  {0, 1, 5}, {0, 5, 4},  {3, 7, 6}, {3, 6, 2},
};
static const float face_colors[6][3] = {
    {1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1},{0,1,1},
};
static const char face_letter[6] = { 'r', 'g', 'b', 'y', 'm', 'c' };

static int classify(uint16_t v)
{
    int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
    const int HI = 27, LO = 4;
    if (r >= HI && g <= LO && b <= LO) return 0;
    if (r <= LO && g >= HI && b <= LO) return 1;
    if (r <= LO && g <= LO && b >= HI) return 2;
    if (r >= HI && g >= HI && b <= LO) return 3;
    if (r >= HI && g <= LO && b >= HI) return 4;
    if (r <= LO && g >= HI && b >= HI) return 5;
    if (r <= LO && g <= LO && b <= LO) return -1;
    return -2;
}

static float tri_area(const struct screen_vertex *a, const struct screen_vertex *b,
                      const struct screen_vertex *c)
{
    float det = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
    return fabsf(det) * 0.5f;
}

struct metrics {
    long blends, misplaced, holes;
    int excess_face; long excess_by;
    long face_ct[6];
    float expected[6];
    int mp_x, mp_y, mp_face, mp_other;
    int hole_x, hole_y;
};

/* Convex hull of the 8 projected vertices (gift wrap), and a
 * margin-inside test. The cube is convex, so the hull IS the true
 * silhouette; any background pixel deeper than HOLE_MARGIN inside it is
 * a rasterization coverage gap. */
#define HOLE_MARGIN 1.5f

static int   hull_n;
static float hull_x[9], hull_y[9];
static float hull_esign[9], hull_elen[9];

static void build_hull(const struct screen_vertex *p)
{
    int start = 0;
    for (int i = 1; i < 8; i++)
        if (p[i].sx < p[start].sx ||
            (p[i].sx == p[start].sx && p[i].sy < p[start].sy))
            start = i;

    hull_n = 0;
    int cur = start;
    do {
        hull_x[hull_n] = p[cur].sx;
        hull_y[hull_n] = p[cur].sy;
        hull_n++;
        int next = -1;
        for (int i = 0; i < 8; i++) {
            if (i == cur) continue;
            if (next < 0) { next = i; continue; }
            float cr = (p[next].sx - p[cur].sx) * (p[i].sy - p[cur].sy)
                     - (p[i].sx - p[cur].sx) * (p[next].sy - p[cur].sy);
            if (cr < 0.0f) {
                next = i;
            } else if (cr == 0.0f) {
                /* collinear: keep the farther point (avoids loops) */
                float dnx = p[next].sx - p[cur].sx, dny = p[next].sy - p[cur].sy;
                float dix = p[i].sx - p[cur].sx,    diy = p[i].sy - p[cur].sy;
                if (dix*dix + diy*diy > dnx*dnx + dny*dny)
                    next = i;
            }
        }
        cur = next;
    } while (cur != start && hull_n < 8);

    /* Precompute each edge's length and its interior sign (the side the
     * centroid is on), so the per-pixel test is orientation-free. */
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < hull_n; i++) { cx += hull_x[i]; cy += hull_y[i]; }
    cx /= hull_n; cy /= hull_n;
    for (int i = 0; i < hull_n; i++) {
        int j = (i + 1) % hull_n;
        float ex = hull_x[j] - hull_x[i], ey = hull_y[j] - hull_y[i];
        hull_elen[i]  = sqrtf(ex*ex + ey*ey);
        float sc = ex * (cy - hull_y[i]) - ey * (cx - hull_x[i]);
        hull_esign[i] = (sc >= 0.0f) ? 1.0f : -1.0f;
    }
}

/* Inside-with-margin: the pixel's signed distance to every hull edge has
 * the interior sign and magnitude >= HOLE_MARGIN. */
static int inside_hull(float qx, float qy)
{
    if (hull_n < 3) return 0;
    for (int i = 0; i < hull_n; i++) {
        if (hull_elen[i] < 1e-3f) continue;
        int j = (i + 1) % hull_n;
        float ex = hull_x[j] - hull_x[i], ey = hull_y[j] - hull_y[i];
        float sq = ex * (qy - hull_y[i]) - ey * (qx - hull_x[i]);
        if (hull_esign[i] * sq < HOLE_MARGIN * hull_elen[i])
            return 0;
    }
    return 1;
}

/* Render the cube at ANGLE (flat colors, back-face cull) into VRAM and
 * measure all three contamination signals. Leaves the rendered frame in VRAM. */
static void measure(struct virge_ctx *vctx, uint8_t *vram, int W, int H,
                    uint32_t stride, float angle, struct metrics *m)
{
    memset(m, 0, sizeof(*m));
    m->mp_x = -1; m->mp_face = -1; m->excess_face = -1; m->hole_x = -1;

    for (int y = 0; y < H; y++) {
        uint16_t *r = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) r[x] = 0;
    }
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    float rot[3][3];
    build_rotation(rot, angle, angle * 0.7f);
    struct screen_vertex projected[8];
    float transformed[8][3];
    for (int i = 0; i < 8; i++) {
        mat3_transform(transformed[i], rot, cube_verts[i]);
        project(&projected[i], transformed[i], W, H, 5.0f);
    }
    build_hull(projected);
    for (int f = 0; f < 6; f++)
        for (int t = 0; t < 2; t++) {
            const int *fc = cube_faces[2*f + t];
            m->expected[f] += tri_area(&projected[fc[0]], &projected[fc[1]], &projected[fc[2]]);
        }

    for (int face = 0; face < 12; face++) {
        int ci = face / 2;
        float fn[3];
        switch (ci) {
        case 0: fn[0]=0;  fn[1]=0;  fn[2]=-1; break;
        case 1: fn[0]=0;  fn[1]=0;  fn[2]=1;  break;
        case 2: fn[0]=-1; fn[1]=0;  fn[2]=0;  break;
        case 3: fn[0]=1;  fn[1]=0;  fn[2]=0;  break;
        case 4: fn[0]=0;  fn[1]=-1; fn[2]=0;  break;
        case 5: fn[0]=0;  fn[1]=1;  fn[2]=0;  break;
        }
        float nv[3]; mat3_transform(nv, rot, fn);
        /* Perspective-correct cull (same as cube.c): visible iff
         * dot(normal, center - eye) < 0; unit-cube face center == normal,
         * eye at origin, cube center at z = +5. The old nv[2] >= 0 test
         * let barely-back-facing slivers through within ~11 deg of
         * edge-on -- the fragment source for the "misplaced" signal. */
        if (nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*(nv[2] + 5.0f) >= 0.0f) continue;
        float r = face_colors[ci][0], g = face_colors[ci][1], b = face_colors[ci][2];
        int i0 = cube_faces[face][0], i1 = cube_faces[face][1], i2 = cube_faces[face][2];
        struct virge_vertex v0 = { .x=projected[i0].sx, .y=projected[i0].sy, .z=projected[i0].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
        struct virge_vertex v1 = { .x=projected[i1].sx, .y=projected[i1].sy, .z=projected[i1].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
        struct virge_vertex v2 = { .x=projected[i2].sx, .y=projected[i2].sy, .z=projected[i2].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
        virge_draw_triangle_gouraud(vctx, v0, v1, v2);
    }
    virge_wait_engine(vctx);

    for (int y = 0; y < H; y++) {
        const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) {
            int c = classify(row[x]);
            if (c >= 0) m->face_ct[c]++;
            else if (c == -2) m->blends++;
            else if (inside_hull((float)x, (float)y)) {
                m->holes++;
                if (m->hole_x < 0) { m->hole_x = x; m->hole_y = y; }
            }
        }
    }

    for (int y = 1; y < H-1; y++) {
        const uint16_t *rm = (const uint16_t *)(vram + (size_t)(y-1) * stride);
        const uint16_t *r0 = (const uint16_t *)(vram + (size_t)y * stride);
        const uint16_t *rp = (const uint16_t *)(vram + (size_t)(y+1) * stride);
        for (int x = 1; x < W-1; x++) {
            int c = classify(r0[x]);
            if (c < 0 || c > 5) continue;
            int nb[4] = { classify(r0[x-1]), classify(r0[x+1]),
                          classify(rm[x]),    classify(rp[x]) };
            int bg_n = 0, diff[6] = {0};
            for (int k = 0; k < 4; k++) {
                if (nb[k] == -1) bg_n++;
                else if (nb[k] >= 0 && nb[k] <= 5 && nb[k] != c) diff[nb[k]]++;
            }
            if (bg_n > 0) continue;
            for (int f = 0; f < 6; f++)
                if (diff[f] >= 3) {
                    m->misplaced++;
                    if (m->mp_x < 0) { m->mp_x = x; m->mp_y = y; m->mp_face = c; m->mp_other = f; }
                    break;
                }
        }
    }

    for (int f = 0; f < 6; f++) {
        if (m->expected[f] < 1.0f) continue;
        long thr = 50 + (long)(m->expected[f] * 0.10f);
        if (m->face_ct[f] > (long)m->expected[f] + thr) {
            long by = m->face_ct[f] - (long)m->expected[f];
            if (by > m->excess_by) { m->excess_by = by; m->excess_face = f; }
        }
    }
}

static void print_rle(const uint8_t *vram, int W, uint32_t stride, int y)
{
    const uint16_t *row0 = (const uint16_t *)(vram + (size_t)y * stride);
    printf("  middle row y=%d RLE: ", y);
    int prev = -99, cnt = 0, emitted = 0;
    for (int x = 0; x <= W; x++) {
        int c = (x < W) ? classify(row0[x]) : -99;
        if (x == W || c != prev) {
            if (cnt > 0) {
                char ch = (prev == -1) ? '.' : (prev == -2) ? '?' : face_letter[prev];
                printf("%c%d ", ch, cnt);
                if (++emitted % 12 == 0) printf("\n          ");
            }
            prev = c; cnt = 1;
        } else cnt++;
    }
    printf("\n  (r=Back g=Front b=Left y=Right m=Bottom c=Top .=bg ?=blend)\n");
}

/* Verdict driver: blends + misplaced only. (excess is reported as info but
 * not verdict-driving -- a legit thin sliver renders far more px than its
 * projected area purely from fill-rule rounding, so excess is unreliable for
 * thin faces and would false-alarm. Use it only as a hint.) */
static long m_tot(const struct metrics *m)
{
    return m->blends + m->misplaced + m->holes;
}

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

int main(int argc, char **argv)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) { fprintf(stderr, "init failed\n"); return 1; }
    /* Match the depth state cube.c runs under (the glue seeds LESS); the
     * virge_init default is LEQUAL, which flips who wins near-tie pixels
     * and would make this probe measure a different config than cube. */
    vctx.z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_LESS | VIRGE_ZUP_ENABLE;
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;
    int N = (argc >= 2) ? atoi(argv[1]) : 36;
    if (N < 4) N = 4;

    printf("\ncubefb: cube framebuffer readback (%dx%d stride %u) -- bypasses the monitor\n",
           W, H, stride);
    printf("Signals: BLENDS (non-face color), MISPLACED (face color inside another face),\n");
    printf("         HOLES (background inside the cube silhouette -- coverage gaps),\n");
    printf("         EXCESS (face renders more px than its projected area, info only).\n");
    printf("Sweeping %d orientations over [0, 2pi)...\n\n", N);

    float worst_angle = 0.0f;
    long worst_tot = -1;
    int contaminated = 0;

    for (int i = 0; i < N; i++) {
        float angle = (float)i * 2.0f * (float)PI / N;
        struct metrics m;
        measure(&vctx, vram, W, H, stride, angle, &m);
        long tot = m_tot(&m);
        if (tot > 0) {
            contaminated++;
            printf("  angle %.2f (%3.0f deg): blends %ld  misplaced %ld  holes %ld  excess %s",
                   angle, angle * 180.0f / (float)PI, m.blends, m.misplaced,
                   m.holes, m.excess_face >= 0 ? "YES" : "no");
            if (m.misplaced)
                printf("  [%c inside %c at (%d,%d)]", face_letter[m.mp_face],
                       face_letter[m.mp_other], m.mp_x, m.mp_y);
            if (m.holes)
                printf("  [hole at (%d,%d)]", m.hole_x, m.hole_y);
            printf("\n");
        }
        if (i == 0 || tot > worst_tot) { worst_tot = tot; worst_angle = angle; }
    }

    printf("\n%d of %d orientations contaminated.\n", contaminated, N);

    /* Re-render the worst and print full detail + the row readback. */
    struct metrics m;
    measure(&vctx, vram, W, H, stride, worst_angle, &m);
    printf("\nWorst orientation: angle %.2f (%.0f deg) -- blends %ld, misplaced %ld, holes %ld, excess %s\n",
           worst_angle, worst_angle * 180.0f / (float)PI, m.blends, m.misplaced,
           m.holes, m.excess_face >= 0 ? "YES" : "no");
    if (m.misplaced)
        printf("  first misplaced: %c at (%d,%d) inside %c\n",
               face_letter[m.mp_face], m.mp_x, m.mp_y, face_letter[m.mp_other]);
    if (m.holes)
        printf("  first hole (background inside the silhouette): (%d,%d) -- "
               "print_rle that row for the notch\n", m.hole_x, m.hole_y);
    if (m.excess_face >= 0)
        printf("  excess: %c rendered %ld vs expected ~%ld (+%ld)\n",
               face_letter[m.excess_face], m.face_ct[m.excess_face],
               (long)m.expected[m.excess_face], m.excess_by);
    printf("  faces[r g b y m c]=%ld %ld %ld %ld %ld %ld\n",
           m.face_ct[0], m.face_ct[1], m.face_ct[2], m.face_ct[3], m.face_ct[4], m.face_ct[5]);
    print_rle(vram, W, stride, H / 2);
    if (m.holes && m.hole_y != H / 2)
        print_rle(vram, W, stride, m.hole_y);

    printf("\n");
    if (contaminated == 0)
        printf("=> All %d orientations CLEAN (0 blends, 0 misplaced, 0 holes). Any remaining\n"
               "   visible artifact is NOT in VRAM -- it is monitor/scanout-side.\n", N);
    else
        printf("=> %d orientation(s) CONTAMINATED -- a real per-frame VRAM artifact (the\n"
               "   worst is detailed above, at the printed angle). NOT the monitor.\n",
               contaminated);

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
