// Writes a coloured STEP assembly for testing the STEP path:
//   gen_test_step OUT.step [copies]
// Each copy is a filleted box (red), a cylinder (blue) and a sphere with one
// face-level colour override.

#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax2.hxx>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: gen_test_step OUT.step [copias]\n");
        return 2;
    }
    int copies = argc > 2 ? std::atoi(argv[2]) : 1;

    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());

    for (int i = 0; i < copies; ++i) {
        double ox = (i % 10) * 120.0, oy = (i / 10) * 120.0;

        TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(ox, oy, 0), 60, 40, 30).Shape();
        BRepFilletAPI_MakeFillet fillet(box);
        for (TopExp_Explorer e(box, TopAbs_EDGE); e.More(); e.Next()) fillet.Add(4.0, TopoDS::Edge(e.Current()));
        TDF_Label boxLabel = shapes->AddShape(fillet.Shape(), false);
        colors->SetColor(boxLabel, Quantity_Color(0.85, 0.2, 0.15, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);

        gp_Ax2 axis(gp_Pnt(ox + 85, oy + 20, 0), gp_Dir(0, 0, 1));
        TDF_Label cylLabel = shapes->AddShape(BRepPrimAPI_MakeCylinder(axis, 15, 50).Shape(), false);
        colors->SetColor(cylLabel, Quantity_Color(0.15, 0.45, 0.9, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);

        TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(ox + 30, oy + 20, 55), 18).Shape();
        TDF_Label sphLabel = shapes->AddShape(sphere, false);
        TopExp_Explorer face(sphere, TopAbs_FACE);
        TDF_Label faceLabel = shapes->AddSubShape(sphLabel, face.Current());
        if (!faceLabel.IsNull()) colors->SetColor(faceLabel, Quantity_Color(0.2, 0.75, 0.3, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);
    }

    STEPCAFControl_Writer writer;
    writer.SetColorMode(true);
    if (!writer.Transfer(doc, STEPControl_AsIs) || writer.Write(argv[1]) != IFSelect_RetDone) {
        std::fprintf(stderr, "error al escribir %s\n", argv[1]);
        return 1;
    }
    std::printf("%s: %d copias\n", argv[1], copies);
    return 0;
}
