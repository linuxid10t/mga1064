/*
 * Minimal OpenGL 1.1-compatible declarations provided by L10GL.
 *
 * This is intentionally a subset.  It contains the fixed-function entry
 * points implemented by src/l10gl_gl.c, rather than pretending that L10GL is
 * a complete desktop OpenGL implementation.
 */

#ifndef L10GL_GL_H
#define L10GL_GL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;

#define GL_FALSE                         0
#define GL_TRUE                          1

#define GL_POINTS                        0x0000
#define GL_LINES                         0x0001
#define GL_LINE_STRIP                    0x0003
#define GL_TRIANGLES                     0x0004
#define GL_TRIANGLE_STRIP                0x0005
#define GL_TRIANGLE_FAN                  0x0006
#define GL_QUADS                         0x0007
#define GL_QUAD_STRIP                    0x0008
#define GL_POLYGON                       0x0009

#define GL_ZERO                          0
#define GL_ONE                           1
#define GL_SRC_COLOR                     0x0300
#define GL_ONE_MINUS_SRC_COLOR           0x0301
#define GL_SRC_ALPHA                     0x0302
#define GL_ONE_MINUS_SRC_ALPHA           0x0303
#define GL_DST_COLOR                     0x0306
#define GL_ONE_MINUS_DST_COLOR           0x0307

#define GL_FRONT                         0x0404
#define GL_BACK                          0x0405
#define GL_FRONT_AND_BACK                0x0408

#define GL_NEVER                         0x0200
#define GL_LESS                          0x0201
#define GL_EQUAL                         0x0202
#define GL_LEQUAL                        0x0203
#define GL_GREATER                       0x0204
#define GL_NOTEQUAL                      0x0205
#define GL_GEQUAL                        0x0206
#define GL_ALWAYS                        0x0207

#define GL_INVALID_ENUM                  0x0500
#define GL_INVALID_VALUE                 0x0501
#define GL_INVALID_OPERATION             0x0502
#define GL_STACK_OVERFLOW                0x0503
#define GL_STACK_UNDERFLOW               0x0504
#define GL_OUT_OF_MEMORY                 0x0505
#define GL_NO_ERROR                      0

#define GL_DEPTH_BUFFER_BIT              0x00000100u
#define GL_COLOR_BUFFER_BIT              0x00004000u

#define GL_CULL_FACE                     0x0B44
#define GL_COLOR_MATERIAL                0x0B57
#define GL_LIGHTING                      0x0B50
#define GL_LIGHT0                        0x4000
#define GL_DEPTH_TEST                    0x0B71
#define GL_NORMALIZE                     0x0BA1
#define GL_BLEND                         0x0BE2
#define GL_TEXTURE_2D                    0x0DE1
#define GL_ALPHA_TEST                    0x0BC0
#define GL_PERSPECTIVE_CORRECTION_HINT   0x0C50

#define GL_UNPACK_ALIGNMENT              0x0CF5

#define GL_UNSIGNED_BYTE                 0x1401
#define GL_FLOAT                         0x1406
#define GL_COLOR_INDEX                   0x1900
#define GL_RGB                           0x1907
#define GL_RGBA                          0x1908
#define GL_ALPHA                         0x1906
#define GL_LUMINANCE                     0x1909
#define GL_INTENSITY                     0x8049
#define GL_RGBA4                         0x805B

#define GL_NEAREST                       0x2600
#define GL_LINEAR                        0x2601
#define GL_NEAREST_MIPMAP_NEAREST        0x2700
#define GL_LINEAR_MIPMAP_NEAREST         0x2701
#define GL_NEAREST_MIPMAP_LINEAR         0x2702
#define GL_LINEAR_MIPMAP_LINEAR          0x2703
#define GL_TEXTURE_MAG_FILTER            0x2800
#define GL_TEXTURE_MIN_FILTER            0x2801
#define GL_TEXTURE_WRAP_S                0x2802
#define GL_TEXTURE_WRAP_T                0x2803
#define GL_CLAMP                         0x2900
#define GL_REPEAT                        0x2901

#define GL_FLAT                          0x1D00
#define GL_SMOOTH                        0x1D01

#define GL_MODELVIEW                     0x1700
#define GL_PROJECTION                    0x1701

#define GL_AMBIENT                       0x1200
#define GL_DIFFUSE                       0x1201
#define GL_POSITION                      0x1203
#define GL_AMBIENT_AND_DIFFUSE           0x1602

/* Texture environment (glTexEnv). GL_BLEND (0x0BE2) is shared with the
 * blend-enable cap, as in the OpenGL registry. */
#define GL_TEXTURE_ENV                   0x2300
#define GL_TEXTURE_ENV_MODE              0x2200
#define GL_MODULATE                      0x2100
#define GL_DECAL                         0x2101
#define GL_REPLACE                       0x1E02

/* Polygon mode. GL_POINT and GL_LINE are accepted only to report
 * GL_INVALID_ENUM until a use case exists; GL_FILL is the drawn mode. */
#define GL_POINT                         0x1B00
#define GL_LINE                          0x1B01
#define GL_FILL                          0x1B02

/* Hints. Implemented as no-ops; L10GL's perspective path is fixed. */
#define GL_DONT_CARE                     0x1100
#define GL_FASTEST                       0x1101
#define GL_NICEST                        0x1102

/* Queries (glGetString / glGetFloatv). */
#define GL_VENDOR                        0x1F00
#define GL_RENDERER                      0x1F01
#define GL_VERSION                       0x1F02
#define GL_EXTENSIONS                    0x1F03
#define GL_MODELVIEW_MATRIX              0x0BA6

struct l10gl_ctx;

/* L10GL owns display/context setup because this project has no GLX/EGL
 * window-system layer. bits_per_pixel is 16 or 32, not bytes per pixel. */
int l10glCreateContext(GLsizei width, GLsizei height, GLint bits_per_pixel);
void l10glDestroyContext(void);
void l10glMakeCurrent(struct l10gl_ctx *ctx);
struct l10gl_ctx *l10glGetCurrentContext(void);
void l10glSwapBuffers(void);

GLenum glGetError(void);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor3fv(const GLfloat *v);
void glColor4fv(const GLfloat *v);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void glNormal3fv(const GLfloat *v);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble z_near, GLdouble z_far);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble z_near, GLdouble z_far);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthRange(GLclampd z_near, GLclampd z_far);

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glCullFace(GLenum mode);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glShadeModel(GLenum mode);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
GLboolean glIsTexture(GLuint texture);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internal_format,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glPixelStorei(GLenum pname, GLint param);
void glFlush(void);
void glFinish(void);

const GLubyte *glGetString(GLenum name);
void glGetFloatv(GLenum pname, GLfloat *params);
void glHint(GLenum target, GLenum mode);

void glColor3ubv(const GLubyte *v);
void glPolygonMode(GLenum face, GLenum mode);
void glDrawBuffer(GLenum mode);
void glReadBuffer(GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *data);

void glAlphaFunc(GLenum func, GLclampf ref);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                     GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels);

#ifdef __cplusplus
}
#endif

#endif /* L10GL_GL_H */
