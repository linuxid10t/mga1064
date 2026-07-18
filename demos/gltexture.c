/* gltexture.c - Phase 4 OpenGL texture-object hardware proof. */

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>

#include "l10gl.h"

#define TEXTURE_SIZE 64

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

static void make_texture(GLubyte pixels[TEXTURE_SIZE * TEXTURE_SIZE * 4])
{
    for (int y = 0; y < TEXTURE_SIZE; y++) {
        for (int x = 0; x < TEXTURE_SIZE; x++) {
            GLubyte *pixel = &pixels[(y * TEXTURE_SIZE + x) * 4];
            int quadrant = (x >= TEXTURE_SIZE / 2) |
                           ((y >= TEXTURE_SIZE / 2) << 1);
            int checker = ((x / 8) ^ (y / 8)) & 1;

            pixel[0] = quadrant == 0 || quadrant == 3 ? 255 : 0;
            pixel[1] = quadrant == 1 || quadrant == 3 ? 255 : 0;
            pixel[2] = quadrant == 2 || quadrant == 3 ? 255 : 0;
            if (checker) {
                pixel[0] /= 2;
                pixel[1] /= 2;
                pixel[2] /= 2;
            }
            pixel[3] = 255;
        }
    }
}

int main(int argc, char **argv)
{
    GLubyte pixels[TEXTURE_SIZE * TEXTURE_SIZE * 4];
    int width = 640;
    int height = 480;
    int bits_per_pixel = 16;
    int frame_limit = frame_limit_from_env();
    int frame = 0;
    GLuint texture;
    struct l10gl_ctx *ctx;

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4)
        bits_per_pixel = atoi(argv[3]);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    printf("L10GL OpenGL Texture Object Demo\n");
    if (l10glCreateContext(width, height, bits_per_pixel) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }
    ctx = l10glGetCurrentContext();
    printf("Selected backend: %s (%dx%d)\n", ctx->backend->name,
           ctx->width, ctx->height);

    make_texture(pixels);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 TEXTURE_SIZE, TEXTURE_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glEnable(GL_TEXTURE_2D);

    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "Failed to create OpenGL texture object.\n");
        l10glDestroyContext();
        return 1;
    }

    glViewport(0, 0, ctx->width, ctx->height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glColor3f(1, 1, 1);

    printf("Rendering repeated RGBA8888 texture... (Ctrl-C to exit)\n");
    while (running) {
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-.85f, -.85f);
        glTexCoord2f(2, 0); glVertex2f( .85f, -.85f);
        glTexCoord2f(2, 2); glVertex2f( .85f,  .85f);
        glTexCoord2f(0, 2); glVertex2f(-.85f,  .85f);
        glEnd();
        glFinish();
        l10glSwapBuffers();
        frame++;

        if (frame_limit && frame >= frame_limit)
            break;
        while (running && !frame_limit)
            usleep(100000);
    }

    printf("Exiting after %d frame%s.\n", frame, frame == 1 ? "" : "s");
    glDeleteTextures(1, &texture);
    l10glDestroyContext();
    return 0;
}
