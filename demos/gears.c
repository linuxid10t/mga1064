/*
 * gears.c - classic three-gear fixed-function OpenGL demonstration.
 *
 * The drawing code uses only the GL 1.1 compatibility surface. Display
 * creation and presentation are deliberately L10GL-specific because L10GL
 * does not provide a window-system binding such as GLX or EGL.
 */

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>

#include "l10gl.h"

#define PI 3.14159265358979323846f

static volatile int running = 1;

static void sighandler(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int frame_limit_from_env(void)
{
    const char *value = getenv("L10GL_FRAMES");
    char *end;
    long limit;

    if (!value || !value[0])
        return 0;
    limit = strtol(value, &end, 10);
    if (*end || limit <= 0 || limit > INT_MAX) {
        fprintf(stderr, "Ignoring invalid L10GL_FRAMES='%s'\n", value);
        return 0;
    }
    return (int)limit;
}

static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
                 GLint teeth, GLfloat tooth_depth, const GLfloat color[4])
{
    GLint i;
    GLfloat angle;
    GLfloat r0 = inner_radius;
    GLfloat r1 = outer_radius - tooth_depth * 0.5f;
    GLfloat r2 = outer_radius + tooth_depth * 0.5f;
    GLfloat da = 2.0f * PI / teeth / 4.0f;
    GLfloat u, v, length;

    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, color);
    glShadeModel(GL_FLAT);

    /* Front face of the gear body. */
    glNormal3f(0, 0, 1);
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * PI / teeth;
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
        if (i < teeth) {
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
            glVertex3f(r1 * cosf(angle + 3 * da),
                       r1 * sinf(angle + 3 * da), width * 0.5f);
        }
    }
    glEnd();

    /* Front faces of the teeth. */
    glBegin(GL_QUADS);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * PI / teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da),
                   width * 0.5f);
        glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da),
                   width * 0.5f);
        glVertex3f(r1 * cosf(angle + 3 * da),
                   r1 * sinf(angle + 3 * da), width * 0.5f);
    }
    glEnd();

    /* Back face of the gear body. */
    glNormal3f(0, 0, -1);
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * PI / teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
        if (i < teeth) {
            glVertex3f(r1 * cosf(angle + 3 * da),
                       r1 * sinf(angle + 3 * da), -width * 0.5f);
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
        }
    }
    glEnd();

    /* Back faces of the teeth. */
    glBegin(GL_QUADS);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * PI / teeth;
        glVertex3f(r1 * cosf(angle + 3 * da),
                   r1 * sinf(angle + 3 * da), -width * 0.5f);
        glVertex3f(r2 * cosf(angle + 2 * da),
                   r2 * sinf(angle + 2 * da), -width * 0.5f);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da),
                   -width * 0.5f);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
    }
    glEnd();

    /* Outward faces of the teeth. */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * PI / teeth;

        glNormal3f(cosf(angle), sinf(angle), 0);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);

        u = r2 * cosf(angle + da) - r1 * cosf(angle);
        v = r2 * sinf(angle + da) - r1 * sinf(angle);
        length = sqrtf(u * u + v * v);
        glNormal3f(v / length, -u / length, 0);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da),
                   width * 0.5f);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da),
                   -width * 0.5f);

        glNormal3f(cosf(angle), sinf(angle), 0);
        glVertex3f(r2 * cosf(angle + 2 * da),
                   r2 * sinf(angle + 2 * da), width * 0.5f);
        glVertex3f(r2 * cosf(angle + 2 * da),
                   r2 * sinf(angle + 2 * da), -width * 0.5f);

        u = r1 * cosf(angle + 3 * da) - r2 * cosf(angle + 2 * da);
        v = r1 * sinf(angle + 3 * da) - r2 * sinf(angle + 2 * da);
        glNormal3f(v, -u, 0);
        glVertex3f(r1 * cosf(angle + 3 * da),
                   r1 * sinf(angle + 3 * da), width * 0.5f);
        glVertex3f(r1 * cosf(angle + 3 * da),
                   r1 * sinf(angle + 3 * da), -width * 0.5f);
    }
    glVertex3f(r1, 0, width * 0.5f);
    glVertex3f(r1, 0, -width * 0.5f);
    glEnd();

    glShadeModel(GL_SMOOTH);

    /* Inner cylinder. */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * PI / teeth;
        glNormal3f(-cosf(angle), -sinf(angle), 0);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
    }
    glEnd();
}

static void draw_gears(GLfloat angle)
{
    static const GLfloat red[4] = { .8f, .1f, 0, 1 };
    static const GLfloat green[4] = { 0, .8f, .2f, 1 };
    static const GLfloat blue[4] = { .2f, .2f, 1, 1 };

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPushMatrix();
    glRotatef(20, 1, 0, 0);
    glRotatef(30, 0, 1, 0);

    glPushMatrix();
    glTranslatef(-3.0f, -2.0f, 0);
    glRotatef(angle, 0, 0, 1);
    gear(1.0f, 4.0f, 1.0f, 20, .7f, red);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1f, -2.0f, 0);
    glRotatef(-2.0f * angle - 9.0f, 0, 0, 1);
    gear(.5f, 2.0f, 2.0f, 10, .7f, green);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1f, 4.2f, 0);
    glRotatef(-2.0f * angle - 25.0f, 0, 0, 1);
    gear(1.3f, 2.0f, .5f, 10, .7f, blue);
    glPopMatrix();

    glPopMatrix();
}

int main(int argc, char **argv)
{
    static const GLfloat light_position[4] = { 5, 5, 10, 0 };
    int width = 640;
    int height = 480;
    int bits_per_pixel = 16;
    int frame_limit = frame_limit_from_env();
    int static_mode = getenv("L10GL_STATIC") != NULL;
    int frame = 0;
    GLfloat angle = 0;
    struct l10gl_ctx *ctx;
    GLfloat aspect;

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4)
        bits_per_pixel = atoi(argv[3]);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    printf("L10GL OpenGL Gears Demo\n");
    if (l10glCreateContext(width, height, bits_per_pixel) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }
    ctx = l10glGetCurrentContext();
    width = ctx->width;
    height = ctx->height;
    printf("Selected backend: %s (%dx%d)\n", ctx->backend->name,
           width, height);

    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    aspect = (GLfloat)height / (GLfloat)width;
    glFrustum(-1, 1, -aspect, aspect, 5, 60);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -40);
    glLightfv(GL_LIGHT0, GL_POSITION, light_position);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1);

    printf("Rendering... (Ctrl-C to exit)%s\n",
           static_mode ? " [L10GL_STATIC: one frame, then idle]" : "");
    while (running) {
        draw_gears(angle);
        glFinish();
        l10glSwapBuffers();
        frame++;

        if ((frame_limit && frame >= frame_limit) || static_mode)
            break;
        angle += 2.0f;
        if (angle >= 360.0f)
            angle -= 360.0f;
        if (frame % 60 == 0)
            printf("Frame %d\n", frame);
    }

    if (static_mode && running) {
        printf("Static frame rendered. Ctrl-C to exit.\n");
        while (running)
            usleep(100000);
    }

    printf("Exiting after %d frame%s.\n", frame, frame == 1 ? "" : "s");
    l10glDestroyContext();
    return 0;
}
