// visor3d: fast preview of STL / 3MF / STEP files.
//
// Start-up path is tuned for latency: the file is parsed on a worker thread
// while the window and GL context are created, Mesa's EGL is forced (no NVIDIA
// driver load, no dGPU wake-up) and libdecor is skipped (KWin decorates).

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../core/camera.h"
#include "../core/loader.h"
#include "../core/parallel.h"
#include "../core/png_write.h"
#include "../core/thumbnail.h"
#include "async_loader.h"
#include "gl_loader.h"
#include "renderer.h"

namespace fs = std::filesystem;
using namespace v3d;

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point gStart = Clock::now();

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(t - gStart).count(); }
double msSinceStart() { return msSince(Clock::now()); }

enum class RunMode {
    Interactive,
    Bench,  // --bench: print start-up timings after the first complete frame and exit
    Shot,   // --shot: like Bench, saving that frame as PNG first
};

struct App {
    App(RunMode mode, std::string shotPath, const LoadOptions& load)
        : mode(mode), shotPath(std::move(shotPath)), loader(load, [] { glfwPostEmptyEvent(); }) {}

    const RunMode mode;
    const std::string shotPath;
    GLFWwindow* window = nullptr;
    Renderer renderer;
    Camera camera;
    AsyncLoader loader;
    std::string currentPath;
    bool dirty = true;
    bool started = false;  // the requested file has been on screen once

    bool rotating = false, panning = false;
    double lastX = 0, lastY = 0;

    double glReadyMs = 0, loadedMs = 0;
};

std::string fileName(const std::string& path) { return fs::path(path).filename().string(); }

std::string groupThousands(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), " ");
    return s;
}

float aspectOf(GLFWwindow* w) {
    int fw, fh;
    glfwGetFramebufferSize(w, &fw, &fh);
    return float(std::max(fw, 1)) / float(std::max(fh, 1));
}

void openFile(App& app, const std::string& path) {
    app.loader.request(path);
    if (app.window) glfwSetWindowTitle(app.window, ("Cargando " + fileName(path) + "…").c_str());
}

void applyResult(App& app, AsyncLoader::Result&& r) {
    app.currentPath = r.path;
    app.loadedMs = msSince(r.finished);
    Scene& s = r.scene;
    if (!s.error.empty()) {
        std::fprintf(stderr, "visor3d: %s: %s\n", r.path.c_str(), s.error.c_str());
        app.renderer.clearScene();
        glfwSetWindowTitle(app.window, (fileName(r.path) + " — error: " + s.error).c_str());
    } else {
        char dims[96];
        std::snprintf(dims, sizeof dims, "%.1f × %.1f × %.1f mm", s.bounds.max[0] - s.bounds.min[0],
                      s.bounds.max[1] - s.bounds.min[1], s.bounds.max[2] - s.bounds.min[2]);
        std::string title = fileName(r.path) + " — " + groupThousands(s.triangleCount()) + " triángulos — " + dims;
        app.camera.fit(s.bounds, aspectOf(app.window));
        app.renderer.setScene(s);
        glfwSetWindowTitle(app.window, title.c_str());
    }
    app.dirty = true;
}

void pollLoader(App& app, std::chrono::milliseconds wait = {}) {
    if (std::optional<AsyncLoader::Result> r = app.loader.poll(wait)) applyResult(app, std::move(*r));
}

// Natural, case-insensitive order: "part2" < "part10".
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            std::string_view na(a.data() + i, ei - i), nb(b.data() + j, ej - j);
            while (na.size() > 1 && na[0] == '0') na.remove_prefix(1);
            while (nb.size() > 1 && nb[0] == '0') nb.remove_prefix(1);
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = ei, j = ej;
            continue;
        }
        int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i, ++j;
    }
    return a.size() - i < b.size() - j;
}

void navigate(App& app, int dir) {
    const std::string& ref = app.loader.busy() ? app.loader.target() : app.currentPath;
    if (ref.empty()) return;
    fs::path cur = fs::absolute(ref);
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cur.parent_path(), ec)) {
        std::string n = e.path().filename().string();
        if (isSupportedFile(n) && e.is_regular_file(ec)) names.push_back(std::move(n));
    }
    if (names.empty()) return;
    std::sort(names.begin(), names.end(), naturalLess);
    std::string curName = cur.filename().string();
    auto it = std::find(names.begin(), names.end(), curName);
    long idx = it == names.end() ? 0 : long(it - names.begin());
    long n = long(names.size());
    idx = ((idx + dir) % n + n) % n;
    std::string next = (cur.parent_path() / names[size_t(idx)]).string();
    if (next != ref) openFile(app, next);
}

App& appOf(GLFWwindow* w) { return *static_cast<App*>(glfwGetWindowUserPointer(w)); }

void onKey(GLFWwindow* w, int key, int, int action, int) {
    if (action == GLFW_RELEASE) return;
    App& app = appOf(w);
    float aspect = aspectOf(w);
    switch (key) {
        case GLFW_KEY_ESCAPE:
        case GLFW_KEY_Q:
        case GLFW_KEY_SPACE: glfwSetWindowShouldClose(w, GLFW_TRUE); return;
        case GLFW_KEY_F:
        case GLFW_KEY_HOME: app.camera.refit(aspect); break;
        case GLFW_KEY_W: app.renderer.wireframe = !app.renderer.wireframe; break;
        case GLFW_KEY_G: app.renderer.grid = !app.renderer.grid; break;
        case GLFW_KEY_LEFT:
        case GLFW_KEY_PAGE_UP: navigate(app, -1); return;
        case GLFW_KEY_RIGHT:
        case GLFW_KEY_PAGE_DOWN: navigate(app, +1); return;
        default: {
            int n = -1;
            if (key >= GLFW_KEY_1 && key <= GLFW_KEY_7) n = key - GLFW_KEY_1;
            if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_7) n = key - GLFW_KEY_KP_1;
            if (n < 0) return;
            app.camera.setPreset(static_cast<ViewPreset>(n), aspect);
        }
    }
    app.dirty = true;
}

void onMouseButton(GLFWwindow* w, int button, int action, int mods) {
    App& app = appOf(w);
    bool down = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        bool pan = down && (mods & GLFW_MOD_SHIFT);
        app.rotating = down && !pan;
        app.panning = pan;
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
        app.panning = down;
        app.rotating = false;
    }
    glfwGetCursorPos(w, &app.lastX, &app.lastY);
}

void onCursor(GLFWwindow* w, double x, double y) {
    App& app = appOf(w);
    double dx = x - app.lastX, dy = y - app.lastY;
    app.lastX = x, app.lastY = y;
    if (app.rotating) {
        app.camera.orbit(float(dx), float(dy));
        app.dirty = true;
    } else if (app.panning) {
        int ww, wh;
        glfwGetWindowSize(w, &ww, &wh);
        app.camera.pan(float(dx), float(dy), float(wh));
        app.dirty = true;
    }
}

void onScroll(GLFWwindow* w, double, double yoff) {
    App& app = appOf(w);
    double x, y;
    int ww, wh;
    glfwGetCursorPos(w, &x, &y);
    glfwGetWindowSize(w, &ww, &wh);
    float ndcX = float(2.0 * x / std::max(ww, 1) - 1.0), ndcY = float(1.0 - 2.0 * y / std::max(wh, 1));
    app.camera.zoom(float(std::pow(0.85, yoff)), ndcX, ndcY, aspectOf(w));
    app.dirty = true;
}

void onDrop(GLFWwindow* w, int count, const char** paths) {
    if (count > 0) openFile(appOf(w), paths[0]);
}

void onDirty(GLFWwindow* w) { appOf(w).dirty = true; }
void onResize(GLFWwindow* w, int, int) { appOf(w).dirty = true; }

[[noreturn]] void quit(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    // Skip freeing GPU buffers and big vectors: the kernel does it faster.
    std::_Exit(code);
}

void saveShot(const std::string& path, int w, int h) {
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4), flipped(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    size_t stride = size_t(w) * 4;
    for (int y = 0; y < h; ++y) std::memcpy(&flipped[size_t(y) * stride], &px[size_t(h - 1 - y) * stride], stride);
    if (!writePng(path, flipped.data(), w, h)) std::fprintf(stderr, "visor3d: no se pudo escribir %s\n", path.c_str());
}

// Start-up frames skip vsync and MSAA for latency. Once the requested file is
// on screen, bench/shot runs report and exit; interactive runs switch to
// steady-state rendering: vsync caps redraws while dragging, and the frame is
// redrawn antialiased.
void finishStartup(App& app) {
    app.started = true;
    if (app.mode != RunMode::Interactive) {
        glFinish();
        std::printf("gpu: %s\nparse %.1f ms | gl listo %.1f ms | primer frame %.1f ms\n",
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)), app.loadedMs, app.glReadyMs,
                    msSinceStart());
        quit(app.renderer.hasScene() ? 0 : 1);
    }
    glfwSwapInterval(1);
    app.renderer.enableMsaa();
    app.dirty = app.renderer.hasScene();
}

void renderFrame(App& app) {
    int fw, fh;
    glfwGetFramebufferSize(app.window, &fw, &fh);
    const bool startupFrame = !app.started && !app.loader.busy();
    app.renderer.draw(app.camera, fw, fh);
    if (startupFrame && app.mode == RunMode::Shot) saveShot(app.shotPath, fw, fh);  // back buffer: before the swap
    glfwSwapBuffers(app.window);
    app.dirty = false;
    if (startupFrame) finishStartup(app);
}

// --info: load without any window and print what was found (tests, scripts).
int printInfo(const std::string& path, const LoadOptions& options) {
    double t0 = msSinceStart();
    Scene s = loadScene(path, options);
    double t1 = msSinceStart();
    if (!s.error.empty()) {
        std::printf("error: %s\n", s.error.c_str());
        return 1;
    }
    size_t painted = 0;
    for (const Mesh& m : s.meshes) painted += !m.triColor.empty();
    std::printf("carga %.1f ms\nmallas %zu (con color por triángulo: %zu)\ninstancias %zu\ntriángulos %llu\n", t1 - t0,
                s.meshes.size(), painted, s.instances.size(), (unsigned long long)s.triangleCount());
    std::printf("bbox %.3f %.3f %.3f .. %.3f %.3f %.3f\npaleta", s.bounds.min[0], s.bounds.min[1], s.bounds.min[2],
                s.bounds.max[0], s.bounds.max[1], s.bounds.max[2]);
    for (uint32_t c : s.palette) std::printf(" #%02X%02X%02X", c & 255, c >> 8 & 255, c >> 16 & 255);
    std::printf("\ncolor por instancia");
    for (const Instance& in : s.instances) std::printf(" %u", in.color);
    std::printf("\n");
    return 0;
}

// --thumbnail: the same image the Dolphin plugin produces.
int writeThumbnail(const std::string& path, int size, const std::string& out, const LoadOptions& options) {
    if (size <= 0 || size > 4096) size = 256;
    std::string err;
    std::optional<Thumbnail> thumb = makeThumbnail(path, size, size, options, &err);
    if (!thumb) {
        std::fprintf(stderr, "visor3d: %s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (const auto* png = std::get_if<EncodedPng>(&*thumb)) {
        FILE* f = std::fopen(out.c_str(), "wb");
        bool ok = f && std::fwrite(png->bytes.data(), 1, png->bytes.size(), f) == png->bytes.size();
        if (f) ok = std::fclose(f) == 0 && ok;
        return ok ? 0 : 1;
    }
    const auto& img = std::get<RgbaImage>(*thumb);
    return writePng(out, img.pixels.data(), img.width, img.height) ? 0 : 1;
}

void usage() {
    std::fprintf(stderr,
                 "uso: visor3d [--bench] [--shot SALIDA.png] [--info] [--thumbnail TAMAÑO SALIDA.png]\n"
                 "              [--no-cache] [--nvidia] ARCHIVO.{stl,3mf,step,stp}\n"
                 "  ratón: izq. orbitar, der./central/shift+izq. desplazar, rueda zoom\n"
                 "  teclas: F encuadrar, 1-7 vistas, W alambre, G rejilla,\n"
                 "          ←/→ archivo anterior/siguiente, Esc/Q/Espacio salir\n");
}

}  // namespace

int main(int argc, char** argv) {
    RunMode mode = RunMode::Interactive;
    LoadOptions load;
    bool useNvidia = false, infoOnly = false;
    int thumbSize = 0;
    std::string path, shotPath, thumbOut;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench")) mode = RunMode::Bench;
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) mode = RunMode::Shot, shotPath = argv[++i];
        else if (!std::strcmp(argv[i], "--nvidia")) useNvidia = true;
        else if (!std::strcmp(argv[i], "--no-cache")) load.useCache = false;
        else if (!std::strcmp(argv[i], "--info")) infoOnly = true;
        else if (!std::strcmp(argv[i], "--thumbnail") && i + 2 < argc) thumbSize = std::atoi(argv[++i]), thumbOut = argv[++i];
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) return usage(), 0;
        else path = argv[i];
    }
    if (path.empty()) return usage(), 2;
    if (infoOnly) return printInfo(path, load);
    if (!thumbOut.empty()) return writeThumbnail(path, thumbSize, thumbOut, load);

    if (!useNvidia) setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 0);
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
    if (!glfwInit()) {
        std::fprintf(stderr, "visor3d: no se pudo inicializar GLFW\n");
        return 1;
    }
    App app(mode, shotPath, load);
    // Threads are started only after glfwInit(): a second thread alive during
    // it makes it ~8 ms slower (glibc switches to multi-threaded locking for
    // the xkb/Wayland set-up). The parser pool goes first, before the window
    // creation dlopen()s the GL driver: created during that, it costs ~10 ms.
    startParallelPool();
    // Parse while the window and GL context are created.
    openFile(app, path);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 0);  // MSAA via an offscreen target, see Renderer::enableMsaa
    glfwWindowHint(GLFW_ALPHA_BITS, 0);  // opaque surface, no compositor blending
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    glfwWindowHint(GLFW_CONTEXT_NO_ERROR, GLFW_TRUE);
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "visor3d");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "visor3d");

    std::string title = "Cargando " + fileName(path) + "…";
    app.window = glfwCreateWindow(1280, 800, title.c_str(), nullptr, nullptr);
    if (!app.window) {
        glfwWindowHint(GLFW_CONTEXT_NO_ERROR, GLFW_FALSE);
        app.window = glfwCreateWindow(1280, 800, title.c_str(), nullptr, nullptr);
    }
    if (!app.window) {
        std::fprintf(stderr, "visor3d: no se pudo crear la ventana OpenGL 3.3\n");
        return 1;
    }
    glfwMakeContextCurrent(app.window);
    glfwSwapInterval(0);  // until finishStartup()
    if (const char* missing = loadGl(glfwGetProcAddress)) {
        std::fprintf(stderr, "visor3d: falta la función GL %s\n", missing);
        return 1;
    }
    // Present a plain frame right away: the window maps sooner, and Mesa only
    // adopts the HiDPI buffer size (known to GLFW already) after the first swap,
    // so the first real frame would otherwise be rendered at the logical size.
    glClearColor(0.2f, 0.215f, 0.24f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glfwSwapBuffers(app.window);
    std::string err;
    if (!app.renderer.init(err)) {
        std::fprintf(stderr, "visor3d: error de shader: %s\n", err.c_str());
        return 1;
    }
    app.glReadyMs = msSinceStart();
    if (app.mode == RunMode::Shot) app.renderer.enableMsaa();  // screenshots: quality over latency

    glfwSetWindowUserPointer(app.window, &app);
    glfwSetKeyCallback(app.window, onKey);
    glfwSetMouseButtonCallback(app.window, onMouseButton);
    glfwSetCursorPosCallback(app.window, onCursor);
    glfwSetScrollCallback(app.window, onScroll);
    glfwSetDropCallback(app.window, onDrop);
    glfwSetWindowRefreshCallback(app.window, onDirty);
    glfwSetFramebufferSizeCallback(app.window, onResize);

    // Give fast formats a moment so the very first frame already shows the model;
    // slow ones (big STEP) get an empty window right away and fill in later.
    pollLoader(app, std::chrono::milliseconds(200));

    while (!glfwWindowShouldClose(app.window)) {
        pollLoader(app);
        if (app.dirty) renderFrame(app);
        glfwWaitEvents();
    }
    quit(0);
}
