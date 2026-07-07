/*
 * cubefb.c - render the cube to VRAM and CPU-read it back, to settle whether
 * the bleedthrough lives in the framebuffer or is introduced by the monitor.
 *
 * seamtest proved the rasterizer is watertight (no shared-edge double-draw)
 * and the Z pipeline is silicon-correct, so the cube's framebuffer should be
 * clean: each face a uniform color with sharp 1px boundaries. This probe
 * renders the cube with FLAT full-saturation face colors (no lighting) at
 * several orientations and CPU-reads the framebuffer -- bypassing the monitor
 * entirely -- classifying every pixel:
 *   - a pure face color (r/g/b/y/m/c),
 *   - background (black),
 *   - OTHER = anything else (a blend / contamination = a real VRAM artifact).
 * It also prints the run-length-encoded color sequence along the middle row,
 * so any bleed BAND (a short run of one face's color inside another) is
 * visible directly.
 *
 * If OTHER == 0 and the middle row shows clean solid runs per face, the
 * framebuffer is correct and the bleed seen on the physical monitor is the
 * monitor's 1.8x non-integer scaling blending the 1px boundaries into color
 * fringes (same root cause as symptom 1) -- not a driver/engine bug.
 *
 * Build: make -B BACKEND=virge cubefb     Run: sudo ./cubefb
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
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

/* Classify a 555 pixel: 0..5 = face index, -1 = background, -2 = other. */
static int classify(uint16_t v)
{
    int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
    const int HI = 27, LO = 4;   /* tolerate minor rounding around 31 / 0 */
    if (r >= HI && g <= LO && b <= LO) return 0;      /* red    / Back   */
    if (r <= LO && g >= HI && b <= LO) return 1;      /* green  / Front  */
    if (r <= LO && g <= LO && b >= HI) return 2;      /* blue   / Left   */
    if (r >= HI && g >= HI && b <= LO) return 3;      /* yellow / Right  */
    if (r >= HI && g <= LO && b >= HI) return 4;      /* magenta/ Bottom */
    if (r <= LO && g >= HI && b >= HI) return 5;      /* cyan   / Top    */
    if (r <= LO && g <= LO && b <= LO) return -1;     /* background */
    return -2;                                        /* contamination */
}
static const char face_letter[7] = { 'r', 'g', 'b', 'y', 'm', 'c', '?' };

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) { fprintf(stderr, "init failed\n"); return 1; }
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;

    printf("\ncubefb: cube framebuffer readback (%dx%d stride %u) -- bypasses the monitor\n\n",
           W, H, stride);

    float angles[] = { 0.3f, 0.6f, 0.9f, 1.2f, 1.5f };
    long worst_other = 0;

    for (size_t a = 0; a < sizeof(angles)/sizeof(angles[0]); a++) {
        float angle = angles[a];
        /* clear FB black + Z far */
        for (int y = 0; y < H; y++) {
            uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
            for (int x = 0; x < W; x++) row[x] = 0;
        }
        virge_clear_z(&vctx, 1.0f);
        virge_wait_engine(&vctx);

        /* transform + draw (flat colors, fixed order, back-face cull) */
        float rot[3][3];
        build_rotation(rot, angle, angle * 0.7f);
        struct screen_vertex projected[8];
        float transformed[8][3];
        for (int i = 0; i < 8; i++) {
            mat3_transform(transformed[i], rot, cube_verts[i]);
            project(&projected[i], transformed[i], W, H, 5.0f);
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
            if (nv[2] >= 0.0f) continue;
            float r = face_colors[ci][0], g = face_colors[ci][1], b = face_colors[ci][2];
            int i0 = cube_faces[face][0], i1 = cube_faces[face][1], i2 = cube_faces[face][2];
            struct virge_vertex v0 = { .x=projected[i0].sx, .y=projected[i0].sy, .z=projected[i0].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            struct virge_vertex v1 = { .x=projected[i1].sx, .y=projected[i1].sy, .z=projected[i1].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            struct virge_vertex v2 = { .x=projected[i2].sx, .y=projected[i2].sy, .z=projected[i2].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            virge_draw_triangle_gouraud(&vctx, v0, v1, v2);
        }
        virge_wait_engine(&vctx);

        /* read back + classify */
        long face_ct[6] = {0}, bg = 0, other = 0;
        int maxrun = 0, run = 0, srx = -1, sry = -1;
        for (int y = 0; y < H; y++) {
            run = 0;
            const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
            for (int x = 0; x < W; x++) {
                int c = classify(row[x]);
                if (c >= 0) face_ct[c]++;
                else if (c == -1) bg++;
                else { other++; if (++run > maxrun) { maxrun = run; srx = x; sry = y; } }
                if (c != -2) run = 0;   /* contiguous-other run only */
            }
        }
        long rendered = face_ct[0]+face_ct[1]+face_ct[2]+face_ct[3]+face_ct[4]+face_ct[5];
        if (other > worst_other) worst_other = other;
        printf("angle %.2f rad: rendered %6ld px, other %4ld, max other-run %d",
               angle, rendered, other, maxrun);
        if (other) printf(" at (%d,%d)", srx, sry);
        printf("   faces[r g b y m c]=%ld %ld %ld %ld %ld %ld\n",
               face_ct[0],face_ct[1],face_ct[2],face_ct[3],face_ct[4],face_ct[5]);

        /* For the middle orientation, print the middle-row color sequence
         * (RLE) so any bleed band inside a face is visible. */
        if (a == 1) {
            int y = H / 2;
            const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
            printf("  middle row y=%d RLE: ", y);
            int prev = -99, cnt = 0, emitted = 0;
            for (int x = 0; x <= W; x++) {
                int c = (x < W) ? classify(row[x]) : -99;
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
    }

    printf("\n");
    if (worst_other == 0)
        printf("=> Framebuffer CLEAN at every orientation (0 contamination, only pure\n"
               "   face colors + background). The bleed you see is NOT in VRAM -- it is\n"
               "   introduced by the monitor's 1.8x non-integer scaling blending the\n"
               "   correct 1px face boundaries into color fringes (same root cause as\n"
               "   symptom 1). Not a driver/engine bug.\n");
    else
        printf("=> Framebuffer CONTAMINATED (%ld other pixels) at some orientation -- a\n"
               "   real VRAM artifact; investigate before blaming the monitor.\n", worst_other);

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
