// libvisor3d_step.so: STEP import through OpenCASCADE (XCAF, with colours).
// Loaded on demand by the core (see step_plugin.cpp); never linked directly.
//
// Output: a single merged mesh in world coordinates with per-vertex normals
// (smooth inside each CAD face, crisp across face boundaries) and a palette
// index per triangle taken from the face/part colours.

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangle.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <RWMesh_FaceIterator.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_DocumentExplorer.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "../core/mesh.h"

using namespace v3d;

namespace {

// VISOR3D_TIMING=1 prints the duration of each import phase to stderr.
struct PhaseTimer {
    bool on = std::getenv("VISOR3D_TIMING") != nullptr;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    void mark(const char* phase) {
        if (!on) return;
        auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "step: %-10s %8.1f ms\n", phase, std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
    }
};

uint32_t toRgba8(const Quantity_ColorRGBA& c) {
    Standard_Real r, g, b;
    c.GetRGB().Values(r, g, b, Quantity_TOC_sRGB);
    auto q = [](Standard_Real v) { return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return rgba(q(r), q(g), q(b));
}

void appendFace(const RWMesh_FaceIterator& f, uint16_t color, Mesh& mesh, std::vector<float>& acc) {
    const int lower = f.NodeLower(), upper = f.NodeUpper();
    const size_t nNodes = size_t(upper - lower + 1);
    const uint32_t base = uint32_t(mesh.vertexCount());

    mesh.positions.reserve(mesh.positions.size() + nNodes * 3);
    for (int n = lower; n <= upper; ++n) {
        gp_Pnt p = f.NodeTransformed(n);
        mesh.positions.insert(mesh.positions.end(), {float(p.X()), float(p.Y()), float(p.Z())});
    }

    // Area-weighted vertex normals within this face only.
    acc.assign(nNodes * 3, 0.f);
    const float* pos = mesh.positions.data() + size_t(base) * 3;
    for (int e = f.ElemLower(); e <= f.ElemUpper(); ++e) {
        int a, b, c;
        f.TriangleOriented(e).Get(a, b, c);
        a -= lower, b -= lower, c -= lower;
        if (a < 0 || b < 0 || c < 0 || size_t(a) >= nNodes || size_t(b) >= nNodes || size_t(c) >= nNodes) continue;
        mesh.indices.insert(mesh.indices.end(), {base + uint32_t(a), base + uint32_t(b), base + uint32_t(c)});
        mesh.triColor.push_back(color);
        const float* pa = pos + a * 3;
        const float* pb = pos + b * 3;
        const float* pc = pos + c * 3;
        float u[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        float v[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        float nx = u[1] * v[2] - u[2] * v[1], ny = u[2] * v[0] - u[0] * v[2], nz = u[0] * v[1] - u[1] * v[0];
        for (int k : {a, b, c}) acc[size_t(k) * 3] += nx, acc[size_t(k) * 3 + 1] += ny, acc[size_t(k) * 3 + 2] += nz;
    }
    mesh.normals.reserve(mesh.normals.size() + nNodes * 3);
    for (size_t i = 0; i < nNodes; ++i) {
        float* n = &acc[i * 3];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float inv = len > 0 ? 1.f / len : 0.f;
        mesh.normals.insert(mesh.normals.end(), {n[0] * inv, n[1] * inv, inv ? n[2] * inv : 1.f});
    }
}

void loadStep(const char* path, Scene& scene) {
    // OCCT prints warnings to stdout by default.
    PhaseTimer timer;
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));

    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 0.001);  // millimetres

    STEPCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(false);
    reader.SetLayerMode(false);
    reader.SetPropsMode(false);
    reader.SetMetaMode(false);
    reader.SetProductMetaMode(false);
    reader.SetSHUOMode(false);
    reader.SetGDTMode(false);
    reader.SetMatMode(false);
    reader.SetViewMode(false);
    timer.mark("init");
    if (reader.ReadFile(path) != IFSelect_RetDone) {
        scene.error = "no se pudo leer el archivo STEP";
        app->Close(doc);
        return;
    }
    timer.mark("leer");
    if (!reader.Transfer(doc)) {
        scene.error = "no se pudo convertir la geometría STEP";
        app->Close(doc);
        return;
    }

    timer.mark("transferir");
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (Standard_Integer i = 1; i <= roots.Length(); ++i) builder.Add(all, XCAFDoc_ShapeTool::GetShape(roots.Value(i)));
    Bnd_Box box;
    BRepBndLib::Add(all, box);
    if (box.IsVoid()) {
        scene.error = "el STEP no contiene sólidos ni superficies";
        app->Close(doc);
        return;
    }
    // Preview-grade tessellation: ~0.15 % of the model size, 17 degrees.
    double diag = std::sqrt(box.SquareExtent());
    BRepMesh_IncrementalMesh(all, std::max(diag * 0.0015, 1e-3), Standard_False, 0.3, Standard_True);

    timer.mark("mallar");
    Mesh mesh;
    std::unordered_map<uint32_t, uint16_t> colorIndex;
    std::vector<float> acc;
    for (XCAFPrs_DocumentExplorer ex(doc, XCAFPrs_DocumentExplorerFlags_OnlyLeafNodes); ex.More(); ex.Next()) {
        const XCAFPrs_DocumentNode& node = ex.Current();
        for (RWMesh_FaceIterator f(node.RefLabel, node.Location, Standard_True, node.Style); f.More(); f.Next()) {
            if (f.IsEmptyMesh()) continue;
            uint16_t ci = 0;
            if (f.HasFaceColor()) {
                uint32_t c = toRgba8(f.FaceColor());
                auto [it, inserted] = colorIndex.try_emplace(c, uint16_t(0));
                if (inserted) it->second = scene.addColor(c);
                ci = it->second;
            }
            appendFace(f, ci, mesh, acc);
        }
    }
    timer.mark("extraer");
    app->Close(doc);

    if (mesh.indices.empty()) {
        scene.error = "el STEP no generó triángulos";
        return;
    }
    // Drop per-triangle colours when the whole model is uncoloured.
    if (std::all_of(mesh.triColor.begin(), mesh.triColor.end(), [](uint16_t c) { return c == 0; })) mesh.triColor = {};
    scene.meshes.push_back(std::move(mesh));
    scene.instances.push_back(Instance{});
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void visor3d_load_step(const char* path, Scene* scene) {
    try {
        loadStep(path, *scene);
    } catch (const Standard_Failure& e) {
        scene->error = std::string("error de OpenCASCADE: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        scene->error = std::string("error al importar STEP: ") + e.what();
    } catch (...) {
        scene->error = "error desconocido al importar STEP";
    }
}
