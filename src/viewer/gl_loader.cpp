#include "gl_loader.h"

#define V3D_GL_DEFINE(type, name) type name = nullptr;
V3D_GL_FUNCTIONS(V3D_GL_DEFINE)
#undef V3D_GL_DEFINE

namespace v3d {

const char* loadGl(GlProcLoader getProc) {
#define V3D_GL_LOAD(type, name)                               \
    name = reinterpret_cast<type>(getProc(#name));            \
    if (!name) return #name;
    V3D_GL_FUNCTIONS(V3D_GL_LOAD)
#undef V3D_GL_LOAD
    return nullptr;
}

}  // namespace v3d
