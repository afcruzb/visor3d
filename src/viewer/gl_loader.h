#pragma once

// Minimal OpenGL 3.3 core loader: only the entry points the viewer uses,
// resolved through the context's GetProcAddress. No libGL link, no glad.

#include <GL/glcorearb.h>

#define V3D_GL_FUNCTIONS(X)                                   \
    X(PFNGLCLEARPROC, glClear)                                \
    X(PFNGLCLEARCOLORPROC, glClearColor)                      \
    X(PFNGLENABLEPROC, glEnable)                              \
    X(PFNGLDISABLEPROC, glDisable)                            \
    X(PFNGLVIEWPORTPROC, glViewport)                          \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                        \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                        \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode)                    \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                \
    X(PFNGLBLENDFUNCPROC, glBlendFunc)                        \
    X(PFNGLGETSTRINGPROC, glGetString)                        \
    X(PFNGLFINISHPROC, glFinish)                              \
    X(PFNGLREADPIXELSPROC, glReadPixels)                      \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                    \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)            \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)            \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)      \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                      \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                      \
    X(PFNGLBUFFERDATAPROC, glBufferData)                      \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)    \
    X(PFNGLCREATESHADERPROC, glCreateShader)                  \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                  \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                    \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)          \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                  \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                  \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)      \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                    \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                  \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)        \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                      \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)      \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                        \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                        \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                        \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)          \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)          \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                      \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                  \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                    \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                    \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)              \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                \
    X(PFNGLTEXBUFFERPROC, glTexBuffer)                        \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)            \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)            \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)      \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)            \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)          \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)          \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)    \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)

#define V3D_GL_DECLARE(type, name) extern type name;
V3D_GL_FUNCTIONS(V3D_GL_DECLARE)
#undef V3D_GL_DECLARE

namespace v3d {

using GlProc = void (*)();
using GlProcLoader = GlProc (*)(const char*);

// Returns the name of the first missing entry point, or nullptr on success.
const char* loadGl(GlProcLoader getProc);

}  // namespace v3d
