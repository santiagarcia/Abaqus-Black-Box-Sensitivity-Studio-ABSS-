#include "abss/viz/scene.hpp"

#include <vtkActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCamera.h>
#include <vtkCellData.h>
#include <vtkCellPicker.h>
#include <vtkCellType.h>
#include <vtkDataSetMapper.h>
#include <vtkDoubleArray.h>
#include <vtkFieldData.h>
#include <vtkIdList.h>
#include <vtkIntArray.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkSphereSource.h>
#include <vtkStringArray.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnstructuredGrid.h>
#include <vtkXMLUnstructuredGridWriter.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace abss::viz {

int vtkCellTypeFor(ElementShape s) {
    switch (s) {
        case ElementShape::Point1: return VTK_VERTEX;
        case ElementShape::Line2: return VTK_LINE;
        case ElementShape::Line3: return VTK_QUADRATIC_EDGE;
        case ElementShape::Tri3: return VTK_TRIANGLE;
        case ElementShape::Tri6: return VTK_QUADRATIC_TRIANGLE;
        case ElementShape::Quad4: return VTK_QUAD;
        case ElementShape::Quad8: return VTK_QUADRATIC_QUAD;
        case ElementShape::Quad9: return VTK_BIQUADRATIC_QUAD;
        case ElementShape::Tet4: return VTK_TETRA;
        case ElementShape::Tet10: return VTK_QUADRATIC_TETRA;
        case ElementShape::Wedge6: return VTK_WEDGE;
        case ElementShape::Wedge15: return VTK_QUADRATIC_WEDGE;
        case ElementShape::Hex8: return VTK_HEXAHEDRON;
        case ElementShape::Hex20: return VTK_QUADRATIC_HEXAHEDRON;
        case ElementShape::Hex27: return VTK_QUADRATIC_HEXAHEDRON;  // drawn with its 20 serendipity nodes
        case ElementShape::Unknown: break;
    }
    return VTK_EMPTY_CELL;
}

namespace {
int vtkNodeCount(int type, ElementShape s) {
    if (s == ElementShape::Hex27) return 20;
    switch (type) {
        case VTK_VERTEX: return 1;
        case VTK_LINE: return 2;
        case VTK_QUADRATIC_EDGE: return 3;
        case VTK_TRIANGLE: return 3;
        case VTK_QUADRATIC_TRIANGLE: return 6;
        case VTK_QUAD: return 4;
        case VTK_QUADRATIC_QUAD: return 8;
        case VTK_BIQUADRATIC_QUAD: return 9;
        case VTK_TETRA: return 4;
        case VTK_QUADRATIC_TETRA: return 10;
        case VTK_WEDGE: return 6;
        case VTK_QUADRATIC_WEDGE: return 15;
        case VTK_HEXAHEDRON: return 8;
        case VTK_QUADRATIC_HEXAHEDRON: return 20;
        default: return 0;
    }
}
}  // namespace

vtkSmartPointer<vtkUnstructuredGrid> buildGrid(const Mesh& mesh) {
    auto g = vtkSmartPointer<vtkUnstructuredGrid>::New();
    vtkNew<vtkPoints> pts;
    pts->SetDataTypeToDouble();
    pts->SetNumberOfPoints(mesh.nodeCount());
    for (std::int32_t i = 0; i < mesh.nodeCount(); ++i) {
        const auto c = mesh.coord(i);
        pts->SetPoint(i, c[0], c[1], mesh.spatialDim == 2 ? 0.0 : c[2]);
    }
    g->SetPoints(pts);
    g->Allocate(mesh.elementCount());
    std::vector<vtkIdType> ids;
    for (const auto& e : mesh.elements) {
        int type = vtkCellTypeFor(e.info.shape);
        const int n = vtkNodeCount(type, e.info.shape);
        if (type == VTK_EMPTY_CELL || n == 0 || static_cast<int>(e.conn.size()) < n) {
            g->InsertNextCell(VTK_EMPTY_CELL, 0, nullptr);
            continue;
        }
        ids.assign(e.conn.begin(), e.conn.begin() + n);
        g->InsertNextCell(type, n, ids.data());
    }
    return g;
}

std::string exportArrayName(const ResultStore& store, const std::string& quantity, const DerivativeSpec& d) {
    if (d.order() == 0) return quantity;
    auto pname = [&](int p) {
        return (p >= 0 && static_cast<size_t>(p) < store.parameters.size()) ? store.parameters[static_cast<size_t>(p)].name
                                                                           : "a" + std::to_string(p);
    };
    const std::string k = d.order() > 1 ? std::to_string(d.order()) : "";
    if (d.isPure()) return "d" + k + "(" + quantity + ")/d(" + pname(d.params[0]) + ")" + k;
    std::string den;
    for (int p : d.params) den += "d(" + pname(p) + ")";
    return "d" + k + "(" + quantity + ")/" + den;
}

void writeFrameVtu(const ResultStore& store, int frame, const std::string& path) {
    if (frame < 0 || static_cast<size_t>(frame) >= store.frames.size()) throw std::out_of_range("writeFrameVtu: frame");
    auto g = buildGrid(store.mesh);
    const auto derivs = store.derivatives();
    const auto& po = store.points.pointOffset;
    for (size_t f = 0; f < store.fields.size(); ++f) {
        const auto& fd = store.fields[f];
        const int nc = fd.componentCount();
        for (const auto& d : derivs) {
            const auto* v = store.get(static_cast<int>(f), d, frame);
            if (!v) continue;
            vtkNew<vtkDoubleArray> a;
            a->SetNumberOfComponents(nc);
            for (int c = 0; c < nc; ++c) a->SetComponentName(c, fd.components[static_cast<size_t>(c)].c_str());
            if (fd.location == FieldLocation::Node || fd.location == FieldLocation::Element) {
                a->SetName(exportArrayName(store, fd.name, d).c_str());
                const vtkIdType n = static_cast<vtkIdType>(v->size() / static_cast<size_t>(nc));
                a->SetNumberOfTuples(n);
                std::copy(v->begin(), v->end(), a->GetPointer(0));
                if (fd.location == FieldLocation::Node) g->GetPointData()->AddArray(a);
                else g->GetCellData()->AddArray(a);
            } else {
                a->SetName((exportArrayName(store, fd.name, d) + " [element average of IPs]").c_str());
                const vtkIdType ne = store.mesh.elementCount();
                a->SetNumberOfTuples(ne);
                for (vtkIdType e = 0; e < ne; ++e)
                    for (int c = 0; c < nc; ++c) {
                        double s = 0;
                        int cnt = 0;
                        if (static_cast<size_t>(e) + 1 < po.size())
                            for (auto p = po[static_cast<size_t>(e)]; p < po[static_cast<size_t>(e) + 1]; ++p) {
                                const double x = (*v)[static_cast<size_t>(p) * static_cast<size_t>(nc) + static_cast<size_t>(c)];
                                if (x == x) {
                                    s += x;
                                    ++cnt;
                                }
                            }
                        a->SetComponent(e, c, cnt ? s / cnt : std::numeric_limits<double>::quiet_NaN());
                    }
                g->GetCellData()->AddArray(a);
            }
        }
    }
    vtkNew<vtkIntArray> labels;
    labels->SetName("Abaqus element label");
    labels->SetNumberOfTuples(store.mesh.elementCount());
    for (std::int32_t e = 0; e < store.mesh.elementCount(); ++e)
        labels->SetValue(e, static_cast<int>(store.mesh.elements[static_cast<size_t>(e)].label));
    g->GetCellData()->AddArray(labels);
    vtkNew<vtkIntArray> nlabels;
    nlabels->SetName("Abaqus node label");
    nlabels->SetNumberOfTuples(store.mesh.nodeCount());
    for (std::int32_t n = 0; n < store.mesh.nodeCount(); ++n)
        nlabels->SetValue(n, static_cast<int>(store.mesh.nodeKeys[static_cast<size_t>(n)].label));
    g->GetPointData()->AddArray(nlabels);

    vtkNew<vtkStringArray> note;
    note->SetName("ABSS");
    const auto& fr = store.frames[static_cast<size_t>(frame)];
    note->InsertNextValue(store.synthetic ? "SYNTHETIC DEMO DATA - computed from an analytic model, not from Abaqus"
                                          : "Backend: " + store.backend);
    note->InsertNextValue("Title: " + store.title);
    note->InsertNextValue("Step " + fr.stepName + ", increment " + std::to_string(fr.increment) + ", total time " +
                          std::to_string(fr.totalTime));
    note->InsertNextValue("Integration-point fields are exported as element averages (display projection).");
    g->GetFieldData()->AddArray(note);

    vtkNew<vtkXMLUnstructuredGridWriter> w;
    w->SetFileName(path.c_str());
    w->SetInputData(g);
    w->SetDataModeToBinary();
    if (w->Write() != 1) throw std::runtime_error("Could not write " + path);
}

// ---------------------------------------------------------------------------------------------------------------

struct Scene::Impl {
    vtkNew<vtkRenderer> renderer;
    vtkSmartPointer<vtkUnstructuredGrid> grid;      // deformed geometry, colored
    vtkSmartPointer<vtkUnstructuredGrid> refGrid;   // undeformed overlay
    vtkSmartPointer<vtkPoints> basePoints;
    vtkSmartPointer<vtkPoints> defPoints;
    vtkNew<vtkDataSetMapper> mapper;
    vtkNew<vtkActor> actor;
    vtkNew<vtkDataSetMapper> refMapper;
    vtkNew<vtkActor> refActor;
    vtkNew<vtkLookupTable> lut;
    vtkNew<vtkScalarBarActor> bar;
    vtkNew<vtkTextActor> footer;
    vtkNew<vtkTextActor> watermark;
    vtkNew<vtkDoubleArray> pointScalars;
    vtkNew<vtkDoubleArray> cellScalars;
    vtkNew<vtkCellPicker> picker;
    std::vector<vtkSmartPointer<vtkActor>> markers;
    std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> markerLabels;
    bool hasMesh = false;
    bool planar = false;
    bool lineOnly = false;
    bool dark = false;
    bool parallel = false;
    double diag = 1.0;
    bool edges = true, wire = false, showRef = false;
    double opacity = 1.0;

    void styleText();
    void applySurfaceStyle();
};

Scene::Scene() : d_(std::make_unique<Impl>()) {
    auto& d = *d_;
    d.pointScalars->SetName("display");
    d.cellScalars->SetName("display");
    d.actor->SetMapper(d.mapper);
    d.refActor->SetMapper(d.refMapper);
    d.refMapper->ScalarVisibilityOff();
    d.refActor->GetProperty()->SetRepresentationToWireframe();
    d.refActor->GetProperty()->SetOpacity(0.45);
    d.refActor->PickableOff();
    d.refActor->VisibilityOff();
    d.mapper->SetLookupTable(d.lut);
    d.mapper->UseLookupTableScalarRangeOn();
    d.mapper->SetInterpolateScalarsBeforeMapping(true);
    d.lut->SetNanColor(0.79, 0.80, 0.82, 1.0);
    d.bar->SetLookupTable(d.lut);
    d.bar->SetNumberOfLabels(5);
    d.bar->SetLabelFormat("%-#.4g");
    d.bar->SetPosition(0.015, 0.10);
    d.bar->SetWidth(0.10);
    d.bar->SetHeight(0.64);
    d.bar->SetMaximumWidthInPixels(110);
    d.bar->SetBarRatio(0.28);
    d.bar->SetDrawNanAnnotation(false);  // "no data" is explained in the footer instead (annotation text clips at the left edge)
    d.bar->UnconstrainedFontSizeOn();
    d.bar->SetTextPad(4);
    d.bar->VisibilityOff();
    d.footer->GetTextProperty()->SetFontSize(12);
    d.footer->GetTextProperty()->SetJustificationToLeft();
    d.footer->GetTextProperty()->SetVerticalJustificationToTop();
    d.footer->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    d.footer->GetPositionCoordinate()->SetValue(0.015, 0.975);
    d.watermark->GetTextProperty()->SetFontSize(15);
    d.watermark->GetTextProperty()->BoldOn();
    d.watermark->GetTextProperty()->SetJustificationToRight();
    d.watermark->GetTextProperty()->SetVerticalJustificationToTop();
    d.watermark->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    d.watermark->GetPositionCoordinate()->SetValue(0.985, 0.975);
    d.renderer->AddActor(d.refActor);
    d.renderer->AddActor(d.actor);
    d.renderer->AddViewProp(d.bar);
    d.renderer->AddViewProp(d.footer);
    d.renderer->AddViewProp(d.watermark);
    d.renderer->GradientBackgroundOn();
    d.picker->SetTolerance(0.004);
    d.picker->PickFromListOn();
    d.picker->AddPickList(d.actor);
    setTheme(false);
}

Scene::~Scene() = default;

vtkRenderer* Scene::renderer() const { return d_->renderer; }
vtkUnstructuredGrid* Scene::grid() const { return d_->grid; }
bool Scene::hasMesh() const { return d_->hasMesh; }
bool Scene::isPlanar() const { return d_->planar; }

void Scene::Impl::styleText() {
    const double t = dark ? 0.90 : 0.11;
    for (auto* p : {bar->GetTitleTextProperty(), bar->GetLabelTextProperty(), bar->GetAnnotationTextProperty(),
                    footer->GetTextProperty()}) {
        p->SetColor(t, t, t);
        p->ShadowOff();
        p->ItalicOff();
    }
    bar->GetTitleTextProperty()->SetFontSize(15);
    bar->GetTitleTextProperty()->BoldOn();
    bar->GetLabelTextProperty()->SetFontSize(13);
    bar->GetLabelTextProperty()->BoldOff();
    bar->GetAnnotationTextProperty()->SetFontSize(12);
    if (dark) watermark->GetTextProperty()->SetColor(0.95, 0.76, 0.48);
    else watermark->GetTextProperty()->SetColor(0.48, 0.27, 0.0);
}

void Scene::Impl::applySurfaceStyle() {
    auto* p = actor->GetProperty();
    p->SetRepresentation(wire ? VTK_WIREFRAME : VTK_SURFACE);
    p->SetEdgeVisibility(edges && !wire);
    if (dark) p->SetEdgeColor(0.055, 0.063, 0.075);
    else p->SetEdgeColor(0.19, 0.20, 0.23);
    p->SetOpacity(opacity);
    p->SetLineWidth(lineOnly ? 5.0f : (wire ? 1.5f : 1.0f));
    p->SetRenderLinesAsTubes(lineOnly);
    p->SetAmbient(0.25);
    p->SetDiffuse(0.8);
    p->SetSpecular(0.05);
    refActor->GetProperty()->SetColor(dark ? 0.75 : 0.35, dark ? 0.77 : 0.37, dark ? 0.80 : 0.40);
    refActor->GetProperty()->SetLineWidth(lineOnly ? 2.0f : 1.0f);
}

void Scene::setTheme(bool dark) {
    auto& d = *d_;
    d.dark = dark;
    if (dark) {
        d.renderer->SetBackground(0x1A / 255.0, 0x1C / 255.0, 0x20 / 255.0);
        d.renderer->SetBackground2(0x2B / 255.0, 0x2F / 255.0, 0x36 / 255.0);
    } else {
        d.renderer->SetBackground(0xE9 / 255.0, 0xEC / 255.0, 0xF0 / 255.0);
        d.renderer->SetBackground2(1.0, 1.0, 1.0);
    }
    d.styleText();
    d.applySurfaceStyle();
}

void Scene::setMesh(const Mesh& mesh) {
    auto& d = *d_;
    d.grid = buildGrid(mesh);
    d.basePoints = d.grid->GetPoints();
    d.defPoints = vtkSmartPointer<vtkPoints>::New();
    d.defPoints->DeepCopy(d.basePoints);
    d.grid->SetPoints(d.defPoints);
    d.refGrid = vtkSmartPointer<vtkUnstructuredGrid>::New();
    d.refGrid->ShallowCopy(d.grid);
    d.refGrid->SetPoints(d.basePoints);
    d.mapper->SetInputData(d.grid);
    d.refMapper->SetInputData(d.refGrid);
    d.pointScalars->SetNumberOfTuples(mesh.nodeCount());
    d.cellScalars->SetNumberOfTuples(mesh.elementCount());
    d.lineOnly = !mesh.elements.empty() &&
                 std::all_of(mesh.elements.begin(), mesh.elements.end(), [](const Element& e) { return e.info.topoDim == 1; });
    double b[6];
    d.grid->GetBounds(b);
    d.planar = (b[5] - b[4]) <= 1e-12 * std::max({1.0, std::abs(b[1] - b[0]), std::abs(b[3] - b[2])});
    d.diag = std::sqrt((b[1] - b[0]) * (b[1] - b[0]) + (b[3] - b[2]) * (b[3] - b[2]) + (b[5] - b[4]) * (b[5] - b[4]));
    if (!(d.diag > 0)) d.diag = 1.0;
    d.hasMesh = true;
    clearScalars();
    setPickMarkers({}, {}, {});
    d.applySurfaceStyle();
    d.actor->GetProperty()->SetColor(0.72, 0.76, 0.82);
}

void Scene::setScalars(const DisplayValues& v) {
    auto& d = *d_;
    if (!d.hasMesh) return;
    auto* pd = d.grid->GetPointData();
    auto* cd = d.grid->GetCellData();
    if (v.values.empty()) {
        clearScalars();
        return;
    }
    if (v.assoc == DataAssoc::Point) {
        if (static_cast<vtkIdType>(v.values.size()) != d.pointScalars->GetNumberOfTuples()) return;
        std::copy(v.values.begin(), v.values.end(), d.pointScalars->GetPointer(0));
        d.pointScalars->Modified();
        if (pd->GetScalars() != d.pointScalars.GetPointer()) pd->SetScalars(d.pointScalars);
        if (cd->GetScalars()) cd->SetScalars(nullptr);
        d.mapper->SetScalarModeToUsePointData();
    } else {
        if (static_cast<vtkIdType>(v.values.size()) != d.cellScalars->GetNumberOfTuples()) return;
        std::copy(v.values.begin(), v.values.end(), d.cellScalars->GetPointer(0));
        d.cellScalars->Modified();
        if (cd->GetScalars() != d.cellScalars.GetPointer()) cd->SetScalars(d.cellScalars);
        if (pd->GetScalars()) pd->SetScalars(nullptr);
        d.mapper->SetScalarModeToUseCellData();
    }
    d.mapper->ScalarVisibilityOn();
    d.grid->Modified();
}

void Scene::clearScalars() {
    auto& d = *d_;
    if (d.grid) {
        d.grid->GetPointData()->SetScalars(nullptr);
        d.grid->GetCellData()->SetScalars(nullptr);
    }
    d.mapper->ScalarVisibilityOff();
    d.bar->VisibilityOff();
}

void Scene::setColoring(const Colormap& map, double lo, double hi, ScaleMode scale, Rgb nan) {
    auto& d = *d_;
    auto& lut = d.lut;
    lut->SetNanColor(nan.r, nan.g, nan.b, 1.0);
    if (scale == ScaleMode::Log && lo > 0 && hi > lo) {
        lut->SetScaleToLog10();
        lut->SetNumberOfTableValues(256);
        for (int i = 0; i < 256; ++i) {
            const auto c = map.sample(i / 255.0);
            lut->SetTableValue(i, c.r, c.g, c.b, 1.0);
        }
    } else if (scale == ScaleMode::SymLog) {
        // Colors follow sign(v) log10(1+|v|/c); the table stays linear in the value so legend labels are true values.
        lut->SetScaleToLinear();
        const int n = 4096;
        const double c = symlogLinearThreshold(lo, hi);
        lut->SetNumberOfTableValues(n);
        for (int i = 0; i < n; ++i) {
            const double v = lo + (i + 0.5) / n * (hi - lo);
            const auto col = map.sample(normalizedPosition(v, lo, hi, ScaleMode::SymLog, c));
            lut->SetTableValue(i, col.r, col.g, col.b, 1.0);
        }
    } else {
        lut->SetScaleToLinear();
        lut->SetNumberOfTableValues(256);
        for (int i = 0; i < 256; ++i) {
            const auto c = map.sample(i / 255.0);
            lut->SetTableValue(i, c.r, c.g, c.b, 1.0);
        }
    }
    lut->SetTableRange(lo, hi);
    lut->Modified();
}

void Scene::setLegend(const std::string& title, const std::string& footer, bool visible) {
    auto& d = *d_;
    d.bar->SetTitle(title.c_str());
    d.bar->SetVisibility(visible && d.hasMesh);
    d.footer->SetInput(footer.c_str());
}

void Scene::setWatermark(const std::string& text) { d_->watermark->SetInput(text.c_str()); }

void Scene::setDisplacement(const std::vector<double>& u, double scale) {
    auto& d = *d_;
    if (!d.hasMesh) return;
    const vtkIdType n = d.basePoints->GetNumberOfPoints();
    if (u.empty() || static_cast<vtkIdType>(u.size()) < 3 * n || scale == 0.0) {
        d.defPoints->DeepCopy(d.basePoints);
    } else {
        double x[3];
        for (vtkIdType i = 0; i < n; ++i) {
            d.basePoints->GetPoint(i, x);
            d.defPoints->SetPoint(i, x[0] + scale * u[static_cast<size_t>(3 * i)], x[1] + scale * u[static_cast<size_t>(3 * i + 1)],
                                  x[2] + scale * u[static_cast<size_t>(3 * i + 2)]);
        }
    }
    d.defPoints->Modified();
    d.grid->Modified();
}

void Scene::setShowUndeformed(bool on) {
    d_->showRef = on;
    d_->refActor->SetVisibility(on && d_->hasMesh);
}
void Scene::setEdges(bool on) {
    d_->edges = on;
    d_->applySurfaceStyle();
}
void Scene::setWireframe(bool on) {
    d_->wire = on;
    d_->applySurfaceStyle();
}
void Scene::setOpacity(double a) {
    d_->opacity = std::clamp(a, 0.05, 1.0);
    d_->applySurfaceStyle();
}

std::array<double, 3> Scene::nodePosition(std::int32_t node) const {
    std::array<double, 3> p{};
    if (d_->defPoints && node >= 0 && node < d_->defPoints->GetNumberOfPoints()) d_->defPoints->GetPoint(node, p.data());
    return p;
}

std::array<double, 3> Scene::cellCenter(std::int32_t cell) const {
    std::array<double, 3> c{};
    if (!d_->grid || cell < 0 || cell >= d_->grid->GetNumberOfCells()) return c;
    vtkNew<vtkIdList> ids;
    d_->grid->GetCellPoints(cell, ids);
    const vtkIdType n = ids->GetNumberOfIds();
    // corner nodes only would be more exact for quadratic cells; the mean of all nodes is inside the cell either way
    for (vtkIdType k = 0; k < n; ++k) {
        double x[3];
        d_->defPoints->GetPoint(ids->GetId(k), x);
        for (int a = 0; a < 3; ++a) c[static_cast<size_t>(a)] += x[a] / double(n);
    }
    return c;
}

void Scene::setPickMarkers(const std::vector<std::array<double, 3>>& pos, const std::vector<std::string>& labels,
                           const std::vector<Rgb>& colors) {
    auto& d = *d_;
    for (auto& a : d.markers) d.renderer->RemoveActor(a);
    for (auto& a : d.markerLabels) d.renderer->RemoveActor(a);
    d.markers.clear();
    d.markerLabels.clear();
    for (size_t i = 0; i < pos.size(); ++i) {
        vtkNew<vtkSphereSource> s;
        s->SetCenter(pos[i][0], pos[i][1], pos[i][2]);
        s->SetRadius(0.012 * d.diag);
        s->SetThetaResolution(20);
        s->SetPhiResolution(14);
        vtkNew<vtkPolyDataMapper> m;
        m->SetInputConnection(s->GetOutputPort());
        auto a = vtkSmartPointer<vtkActor>::New();
        a->SetMapper(m);
        const Rgb c = i < colors.size() ? colors[i] : Rgb{0.12, 0.37, 0.75};
        a->GetProperty()->SetColor(c.r, c.g, c.b);
        a->GetProperty()->SetAmbient(0.4);
        a->PickableOff();
        d.renderer->AddActor(a);
        d.markers.push_back(a);
        if (i < labels.size()) {
            auto t = vtkSmartPointer<vtkBillboardTextActor3D>::New();
            t->SetInput(labels[i].c_str());
            t->SetPosition(pos[i][0], pos[i][1], pos[i][2]);
            t->SetDisplayOffset(10, 8);
            t->GetTextProperty()->SetFontSize(16);
            t->GetTextProperty()->BoldOn();
            t->GetTextProperty()->SetColor(c.r, c.g, c.b);
            t->GetTextProperty()->SetBackgroundColor(d.dark ? 0.12 : 1.0, d.dark ? 0.13 : 1.0, d.dark ? 0.15 : 1.0);
            t->GetTextProperty()->SetBackgroundOpacity(0.8);
            t->PickableOff();
            d.renderer->AddActor(t);
            d.markerLabels.push_back(t);
        }
    }
}

void Scene::applyView(View v) {
    auto& d = *d_;
    auto* cam = d.renderer->GetActiveCamera();
    if (!d.hasMesh) return;
    double b[6];
    d.grid->GetBounds(b);
    const double c[3] = {(b[0] + b[1]) / 2, (b[2] + b[3]) / 2, (b[4] + b[5]) / 2};
    double dir[3] = {0, 0, 1}, up[3] = {0, 1, 0};
    switch (v) {
        case View::Fit:
            d.renderer->ResetCamera();
            d.renderer->ResetCameraClippingRange();
            return;
        case View::Iso:
            dir[0] = 1;
            dir[1] = 1;
            dir[2] = 1;
            if (d.planar) {  // a 2D model seen from slightly above keeps its plane readable
                dir[0] = 0.35;
                dir[1] = -0.55;
                dir[2] = 1;
            }
            up[0] = 0;
            up[1] = d.planar ? 1 : 1;
            up[2] = 0;
            break;
        case View::XY: break;
        case View::XZ:
            dir[0] = 0;
            dir[1] = -1;
            dir[2] = 0;
            up[0] = 0;
            up[1] = 0;
            up[2] = 1;
            break;
        case View::YZ:
            dir[0] = 1;
            dir[1] = 0;
            dir[2] = 0;
            up[0] = 0;
            up[1] = 0;
            up[2] = 1;
            break;
    }
    cam->SetFocalPoint(c);
    cam->SetPosition(c[0] + dir[0] * d.diag * 3, c[1] + dir[1] * d.diag * 3, c[2] + dir[2] * d.diag * 3);
    cam->SetViewUp(up);
    d.renderer->ResetCamera();
    // Axis-aligned views of flat or box-like models: fit the projected extent instead of the bounding sphere.
    if (v == View::XY || v == View::XZ || v == View::YZ) {
        double w = 0, h = 0;
        if (v == View::XY) {
            w = b[1] - b[0];
            h = b[3] - b[2];
        } else if (v == View::XZ) {
            w = b[1] - b[0];
            h = b[5] - b[4];
        } else {
            w = b[3] - b[2];
            h = b[5] - b[4];
        }
        double aspect = 1.5;
        if (auto* win = d.renderer->GetRenderWindow()) {
            const int* sz = win->GetSize();
            if (sz && sz[0] > 0 && sz[1] > 0) aspect = double(sz[0]) / double(sz[1]);
        }
        const double half = 0.5 * std::max(h, w / aspect) * 1.32;  // margin for legend, notes and labels
        if (half > 0 && cam->GetParallelProjection()) cam->SetParallelScale(half);
        else if (!cam->GetParallelProjection()) cam->Zoom(1.25);
    } else if (v == View::Iso) {
        cam->Zoom(1.15);
    }
    d.renderer->ResetCameraClippingRange();
}

void Scene::setParallelProjection(bool on) {
    d_->parallel = on;
    d_->renderer->GetActiveCamera()->SetParallelProjection(on);
    d_->renderer->ResetCameraClippingRange();
}
bool Scene::parallelProjection() const { return d_->parallel; }

Scene::Pick Scene::pick(int x, int y) {
    Pick p;
    auto& d = *d_;
    if (!d.hasMesh) return p;
    if (d.picker->Pick(x, y, 0, d.renderer) == 0) return p;
    const vtkIdType cell = d.picker->GetCellId();
    if (cell < 0) return p;
    double w[3];
    d.picker->GetPickPosition(w);
    p.hit = true;
    p.cell = static_cast<std::int32_t>(cell);
    p.position = {w[0], w[1], w[2]};
    p.node = nearestNodeOfCell(p.cell, p.position);
    return p;
}

std::int32_t Scene::nearestNodeOfCell(std::int32_t cell, const std::array<double, 3>& p) const {
    if (!d_->grid || cell < 0 || cell >= d_->grid->GetNumberOfCells()) return -1;
    vtkNew<vtkIdList> ids;
    d_->grid->GetCellPoints(cell, ids);
    double best = 1e300;
    std::int32_t node = -1;
    for (vtkIdType k = 0; k < ids->GetNumberOfIds(); ++k) {
        double x[3];
        d_->defPoints->GetPoint(ids->GetId(k), x);
        const double dd = (x[0] - p[0]) * (x[0] - p[0]) + (x[1] - p[1]) * (x[1] - p[1]) + (x[2] - p[2]) * (x[2] - p[2]);
        if (dd < best) {
            best = dd;
            node = static_cast<std::int32_t>(ids->GetId(k));
        }
    }
    return node;
}

}  // namespace abss::viz
