// GUI-layer tests (headless; QT_QPA_PLATFORM=offscreen): controller state wiring, display projections, colormaps,
// ranges, labels, pick -> history data, exports, VTU, and save/reopen through the controller.
#include "abss/core/project_io.hpp"
#include "abss/gui/controller.hpp"
#include "abss/viz/colormap.hpp"
#include "abss/viz/display.hpp"
#include "abss/viz/scene.hpp"

#include <doctest/doctest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <vtkCellType.h>
#include <vtkUnstructuredGrid.h>

#include <cmath>

using namespace abss;
using namespace abss::gui;

namespace {
QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "test_gui";
    static char* argv[] = {name, nullptr};
    static QCoreApplication a(argc, argv);
    return a;
}

int paramIndex(const ResultStore& s, const std::string& name) {
    for (size_t i = 0; i < s.parameters.size(); ++i)
        if (s.parameters[i].name == name) return static_cast<int>(i);
    return -1;
}

/// Elastic two-bar truss, first order, validated: the reference used by most checks below.
Controller& trussController() {
    app();
    static Controller c;
    static bool ready = false;
    if (!ready) {
        QString err;
        REQUIRE(c.loadDemo(DemoModel::TwoBarTrussElastic, &err));
        RunSettings s;
        s.parameters = defaultParameterIndices(DemoModel::TwoBarTrussElastic, *c.model());
        s.maxOrder = 2;
        s.validate = true;
        REQUIRE(c.runSync(s, &err));
        ready = true;
    }
    return c;
}
}  // namespace

TEST_CASE("colormaps: required built-ins, endpoints, diverging midpoint") {
    for (const char* n : {"Viridis", "Plasma", "Inferno", "Turbo", "Cool-warm", "Seismic", "Blue-white-red", "Grayscale"})
        CHECK_MESSAGE(viz::findColormap(n) != nullptr, n);
    const auto* v = viz::findColormap("Viridis");
    REQUIRE(v);
    CHECK(v->table.size() == 256);
    CHECK(v->sample(0).r == doctest::Approx(0.267).epsilon(0.03));
    CHECK(v->sample(0).b == doctest::Approx(0.329).epsilon(0.03));
    CHECK(v->sample(1).g == doctest::Approx(0.906).epsilon(0.03));
    const auto* cw = viz::findColormap("Cool-warm");
    REQUIRE(cw);
    CHECK(cw->diverging);
    const auto mid = cw->sample(0.5);
    CHECK(std::abs(mid.r - mid.g) < 0.06);
    CHECK(std::abs(mid.g - mid.b) < 0.08);
    CHECK(mid.r > 0.8);
    CHECK(cw->sample(0).b > cw->sample(0).r);  // blue end
    CHECK(cw->sample(1).r > cw->sample(1).b);  // red end
    CHECK(viz::findColormap("Seismic")->sample(0.5).r == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("custom palette parsing: r,g,b 0-255, t,r,g,b, hex, and errors") {
    auto a = viz::parseColormapText("0,0,255\n255,255,255\n255,0,0\n", "bwr");
    CHECK(a.custom);
    CHECK(a.diverging);
    CHECK(a.sample(0).b == doctest::Approx(1.0));
    CHECK(a.sample(1).r == doctest::Approx(1.0));
    auto b = viz::parseColormapText("# my map\n0.0 0 0 0\n0.25 1 0 0\n1.0 1 1 1\n", "stops");
    CHECK(b.sample(0.25).r == doctest::Approx(1.0).epsilon(0.02));
    CHECK(b.sample(0.25).g == doctest::Approx(0.0).epsilon(0.02));
    auto c = viz::parseColormapText("#000000\n#FFFFFF\n", "hex");
    CHECK(c.sample(1).g == doctest::Approx(1.0));
    CHECK_THROWS_AS(viz::parseColormapText("1,2\n3,4\n", "bad"), std::invalid_argument);
    CHECK_THROWS_AS(viz::parseColormapText("0,0,0\n", "short"), std::invalid_argument);
    CHECK_THROWS_AS(viz::parseColormapText("0.5 0 0 0\n0.2 1 1 1\n", "order"), std::invalid_argument);
    viz::registerCustomColormap(a);
    CHECK(viz::findColormap("bwr") != nullptr);
}

TEST_CASE("color ranges: centered, constant, log validity, symmetric-log mapping") {
    viz::Range r;
    r.include(-2.0);
    r.include(5.0);
    r.include(std::nan(""));
    auto c = viz::resolveRange(r, true);
    CHECK(c.lo == doctest::Approx(-5));
    CHECK(c.hi == doctest::Approx(5));
    auto u = viz::resolveRange(r, false);
    CHECK(u.lo == doctest::Approx(-2));
    CHECK_FALSE(viz::logScaleValid(r));
    viz::Range k;
    k.include(3.2e-5);
    auto kc = viz::resolveRange(k, false);
    CHECK(kc.constant);
    CHECK(kc.constantValue == doctest::Approx(3.2e-5));
    CHECK(kc.hi > kc.lo);
    CHECK(viz::logScaleValid(k));
    CHECK(viz::resolveRange(viz::Range{}, false).empty);
    const double th = viz::symlogLinearThreshold(-10, 10);
    CHECK(th == doctest::Approx(0.01));
    for (double v : {0.001, 0.5, 3.0, 9.0}) {
        const double p = viz::normalizedPosition(v, -10, 10, viz::ScaleMode::SymLog, th);
        const double q = viz::normalizedPosition(-v, -10, 10, viz::ScaleMode::SymLog, th);
        CHECK(p + q == doctest::Approx(1.0));
        CHECK(p > 0.5);
    }
    CHECK(viz::normalizedPosition(0, -10, 10, viz::ScaleMode::SymLog, th) == doctest::Approx(0.5));
    // symlog spreads small values more than linear
    CHECK(viz::normalizedPosition(0.1, -10, 10, viz::ScaleMode::SymLog, th) > viz::normalizedPosition(0.1, -10, 10, viz::ScaleMode::Linear, th));
    CHECK(viz::normalizedPosition(10, 1, 100, viz::ScaleMode::Log, 0) == doctest::Approx(0.5));
}

TEST_CASE("controller: demo load, run in the GUI path, analytic derivative through display + pick history") {
    auto& c = trussController();
    REQUIRE(c.results());
    const auto& s = *c.results();
    CHECK(s.synthetic);
    CHECK(c.source() == Controller::Source::Demo);
    const int E = paramIndex(s, "E");
    REQUIRE(E >= 0);
    viz::DisplaySpec spec;
    spec.output = {s.fieldIndex("U"), 0, false};
    spec.derivative = DerivativeSpec::pure(E, 1);
    // Click on node index 1 (the middle node): history of dU1/dE equals -F_i L / (2 E^2 A) (see tests/sensitivity).
    const auto h = viz::historyAt(s, spec, {viz::DataAssoc::Point, 1});
    REQUIRE(h.size() == s.frames.size());
    CHECK(std::isnan(h[0]));  // derivative series start at increment 1
    const double Ey = 200000, A = 2, L = 10, F = 1000;
    for (int i = 1; i <= 4; ++i) {
        const double Fi = 0.25 * i * F;
        CHECK(h[static_cast<size_t>(i)] == doctest::Approx(-Fi * L / (2 * Ey * Ey * A)).epsilon(1e-6));
        const auto dv = viz::extractDisplay(s, spec, i);
        CHECK(dv.available);
        CHECK(dv.assoc == viz::DataAssoc::Point);
        CHECK(dv.values[1] == doctest::Approx(h[static_cast<size_t>(i)]));
    }
    // Quality: validated against full reruns (orders 1-2), labelled; magnitude not offered for derivatives.
    CHECK(s.quality.at(DerivativeSpec::pure(E, 1)).status == QualityStatus::Validated);
    CHECK_FALSE(viz::outputAvailable(s, {s.fieldIndex("U"), 0, true}, DerivativeSpec::pure(E, 1)));
    CHECK(viz::outputAvailable(s, {s.fieldIndex("U"), 0, true}, DerivativeSpec::nominal()));
    // Range over all increments of a sensitivity contains the analytic extreme.
    const auto r = viz::rangeOverAllFrames(s, spec);
    CHECK(r.lo <= -F * L / (2 * Ey * Ey * A) * (1 - 1e-6));
}

TEST_CASE("labels: notation, units, question sentence, frame label") {
    auto& c = trussController();
    const auto& s = *c.results();
    const int E = paramIndex(s, "E");
    const viz::OutputChoice u1{s.fieldIndex("U"), 0, false};
    CHECK(viz::notationLabel(s, u1, DerivativeSpec::nominal()) == "U1 (nominal)");
    CHECK(viz::notationLabel(s, u1, DerivativeSpec::pure(E, 1)) == "dU1/dE");
    CHECK(viz::notationLabel(s, u1, DerivativeSpec::pure(E, 2)) == "d²U1/dE²");
    CHECK(viz::unitsLine(s, u1, DerivativeSpec::pure(E, 1)) == "model units of U1 per model unit of E");
    CHECK(viz::unitsLine(s, u1, DerivativeSpec::pure(E, 2)) == "model units of U1 per (model unit of E)²");
    const auto q = viz::questionSentence(s, u1, DerivativeSpec::pure(E, 1), viz::DataAssoc::Point);
    CHECK(q.find("How much does the displacement in X (U1) change when") == 0);
    CHECK(viz::frameLabel(s, 2).find("Increment 2 of 4") == 0);
    CHECK(viz::frameLabel(s, 0).find("(initial state)") != std::string::npos);
    CHECK(viz::formatValue(std::nan("")) == "no data");
    CHECK(viz::formatValue(-1.23456e-7) == "-1.235e-07");
}

TEST_CASE("integration-point data: element average is labelled, single point is raw") {
    app();
    Controller c;
    QString err;
    REQUIRE(c.loadDemo(DemoModel::PlateWithHole, &err));
    const auto& s = *c.model();
    const int mis = s.fieldIndex("S_Mises");
    REQUIRE(mis >= 0);
    const int frame = static_cast<int>(s.frames.size()) - 1;
    viz::DisplaySpec spec;
    spec.output = {mis, 0, false};
    const auto avg = viz::extractDisplay(s, spec, frame);
    CHECK(avg.assoc == viz::DataAssoc::Cell);
    CHECK(avg.projectionNote.find("element average of integration points") != std::string::npos);
    const auto* raw = s.get(mis, DerivativeSpec::nominal(), frame);
    REQUIRE(raw);
    const auto& po = s.points.pointOffset;
    for (int e : {0, 7, 41}) {
        double m = 0;
        for (auto p = po[static_cast<size_t>(e)]; p < po[static_cast<size_t>(e) + 1]; ++p) m += (*raw)[static_cast<size_t>(p)];
        m /= double(po[static_cast<size_t>(e) + 1] - po[static_cast<size_t>(e)]);
        CHECK(avg.values[static_cast<size_t>(e)] == doctest::Approx(m));
    }
    spec.projection = viz::IpProjection::SinglePoint;
    spec.ipNumber = 3;
    const auto one = viz::extractDisplay(s, spec, frame);
    CHECK(one.projectionNote.find("integration point 3") != std::string::npos);
    CHECK(one.values[7] == doctest::Approx((*raw)[static_cast<size_t>(po[7] + 2)]));
    // History at an element uses the same projection as the colors.
    const auto h = viz::historyAt(s, spec, {viz::DataAssoc::Cell, 7});
    CHECK(h[static_cast<size_t>(frame)] == doctest::Approx(one.values[7]));
    CHECK(viz::maxIntegrationPoints(s) == 4);
    // Node picks do not apply to element data.
    CHECK(std::isnan(viz::historyAt(s, spec, {viz::DataAssoc::Point, 7})[1]));
}

TEST_CASE("save and reopen through the controller; CSV exports carry the synthetic marker") {
    auto& c = trussController();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("truss.abss");
    QString err;
    REQUIRE(c.saveProject(path, &err));
    Controller c2;
    REQUIRE(c2.loadProject(path, &err));
    CHECK(c2.source() == Controller::Source::Project);
    REQUIRE(c2.results());
    const auto& a = *c.results();
    const auto& b = *c2.results();
    CHECK(b.synthetic);
    CHECK(b.frames.size() == a.frames.size());
    const int E = paramIndex(b, "E");
    viz::DisplaySpec spec;
    spec.output = {b.fieldIndex("U"), 0, false};
    spec.derivative = DerivativeSpec::pure(E, 1);
    const auto ha = viz::historyAt(a, spec, {viz::DataAssoc::Point, 1});
    const auto hb = viz::historyAt(b, spec, {viz::DataAssoc::Point, 1});
    for (size_t i = 1; i < ha.size(); ++i) CHECK(hb[i] == doctest::Approx(ha[i]));
    CHECK(b.quality.at(spec.derivative).status == QualityStatus::Validated);
    // Setting up a new run is not possible from a results file (no oracle): the run reports it plainly.
    RunSettings s;
    s.parameters = {E};
    CHECK_FALSE(c2.runSync(s, &err));
    CHECK(err.contains("project file"));

    const QString csv = historyCsv(b, {{viz::DataAssoc::Point, 1}, {viz::DataAssoc::Point, 2}}, {spec});
    CHECK(csv.contains("SYNTHETIC DEMO DATA"));
    CHECK(csv.contains("# Point A: node 2"));
    CHECK(csv.contains("frame,step,increment,step_time,total_time,A: dU1/dE at node 2,B: dU1/dE at node 3"));
    CHECK(csv.contains("quality: Validated"));
    const auto lines = csv.split('\n', Qt::SkipEmptyParts);
    int dataRows = 0;
    for (const auto& l : lines)
        if (!l.startsWith('#') && !l.startsWith("frame")) ++dataRows;
    CHECK(dataRows == static_cast<int>(b.frames.size()));
    const QString fcsv = frameCsv(b, spec, 2);
    CHECK(fcsv.contains("node_label,instance,value"));
    CHECK(fcsv.split('\n', Qt::SkipEmptyParts).size() >= b.mesh.nodeCount() + 3);
}

TEST_CASE("cancellation: a cancelled run keeps the nominal results and is reported as stopped") {
    app();
    Controller c;
    QString err;
    REQUIRE(c.loadDemo(DemoModel::TwoBarTrussPlastic, &err));
    RunSettings s;
    s.parameters = defaultParameterIndices(DemoModel::TwoBarTrussPlastic, *c.model());
    s.maxOrder = 1;
    RunControl ctl;
    std::vector<std::pair<std::string, double>> progress;
    ctl.progress = [&](const std::string& st, double f) { progress.push_back({st, f}); };
    ctl.cancel = true;
    const auto out = Controller::executeRun(Controller::Source::Demo, DemoModel::TwoBarTrussPlastic, {}, c.model(), s, ctl);
    CHECK(out.cancelled);
    CHECK_FALSE(out.ok);
    REQUIRE(out.store);
    CHECK(out.store->has(out.store->fieldIndex("U"), DerivativeSpec::nominal()));
    CHECK_FALSE(out.store->has(out.store->fieldIndex("U"), DerivativeSpec::pure(s.parameters.front(), 1)));
    REQUIRE_FALSE(progress.empty());
    for (size_t i = 1; i < progress.size(); ++i) CHECK(progress[i].second >= progress[i - 1].second - 1e-12);  // monotone
    CHECK(progress.back().first == "Stopped");
}

TEST_CASE("output filtering, cost preview and parameter rename") {
    auto& c = trussController();
    const auto& s = *c.results();
    auto f = Controller::filterFields(s, {"U", "S"});
    REQUIRE(f->fields.size() == 2);
    CHECK(f->fields[1].name == "S");
    const int E = paramIndex(s, "E");
    CHECK(f->has(f->fieldIndex("S"), DerivativeSpec::pure(E, 1)));
    CHECK(f->quality.size() == s.quality.size());

    RunSettings rs;
    rs.parameters = {0, 1};
    rs.maxOrder = 2;
    rs.validate = true;
    const auto est = c.estimate(rs);
    const int frames = static_cast<int>(c.model()->frames.size()) - 1;
    CHECK(est.engineEvaluations == estimateOracleEvaluations(frames, 2, 2, defaultHalfWidth(2), true));
    CHECK(est.validationAnalyses == 1 + 2 * 2 * defaultHalfWidth(2));
    CHECK(est.lines.join(" ").contains("every accepted increment"));
    rs.maxOrder = 3;
    CHECK(c.estimate(rs).warnings.join(" ").contains("experimental"));

    app();
    Controller c3;
    QString err;
    REQUIRE(c3.loadDemo(DemoModel::TwoBarTrussElastic, &err));
    c3.setParameterName(0, "Young");
    CHECK(c3.model()->parameters[0].name == "Young");
    // INP-style names "MATERIAL: symbol" use the short symbol in notation, disambiguated only when needed.
    c3.setParameterName(0, "STEEL: E");
    const viz::OutputChoice u1{c3.model()->fieldIndex("U"), 0, false};
    CHECK(viz::notationLabel(*c3.model(), u1, DerivativeSpec::pure(0, 1)) == "dU1/dE");
    c3.setParameterName(1, "ALU: E");
    CHECK(viz::notationLabel(*c3.model(), u1, DerivativeSpec::pure(0, 1)) == "dU1/dE(STEEL)");
}

TEST_CASE("VTK grid, scene data swaps and VTU export") {
    auto& c = trussController();
    const auto& s = *c.results();
    auto g = viz::buildGrid(s.mesh);
    CHECK(g->GetNumberOfCells() == s.mesh.elementCount());
    CHECK(g->GetCellType(0) == VTK_LINE);
    CHECK(viz::vtkCellTypeFor(ElementShape::Quad8) == VTK_QUADRATIC_QUAD);
    CHECK(viz::vtkCellTypeFor(ElementShape::Hex20) == VTK_QUADRATIC_HEXAHEDRON);
    CHECK(viz::vtkCellTypeFor(ElementShape::Tet10) == VTK_QUADRATIC_TETRA);

    viz::Scene scene;
    scene.setMesh(s.mesh);
    CHECK(scene.hasMesh());
    viz::DisplaySpec spec;
    spec.output = {s.fieldIndex("U"), 0, false};
    const auto dv = viz::extractDisplay(s, spec, 4);
    scene.setScalars(dv);
    scene.setDisplacement(viz::nodalDisplacement(s, 4), 100.0);
    const auto p = scene.nodePosition(1);
    CHECK(p[0] == doctest::Approx(s.mesh.coord(1)[0] + 100.0 * dv.values[1]));
    CHECK(scene.nearestNodeOfCell(0, scene.nodePosition(1)) == 1);

    QTemporaryDir dir;
    const std::string path = dir.filePath("frame.vtu").toStdString();
    viz::writeFrameVtu(s, 3, path);
    QFile f(QString::fromStdString(path));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    CHECK(bytes.contains("d(U)/d(E)"));
    CHECK(bytes.contains("Abaqus node label"));
}

// Running an INP needs a licensed Abaqus (minutes, uses a licence token); that path is exercised by the "abaqus"
// labelled tests and manually through the GUI, not here.
TEST_CASE("INP mode: mesh, parameters with provenance, cost preview without a nominal run") {
    app();
    Controller c;
    QString err;
#ifdef ABSS_EXAMPLES_DIR
    const QString inp = QString(ABSS_EXAMPLES_DIR) + "/models/cps4_patch.inp";
#else
    const QString inp = QFileInfo(QString(__FILE__)).absoluteDir().filePath("../../examples/models/cps4_patch.inp");
#endif
    REQUIRE_MESSAGE(c.loadInp(inp, &err), err.toStdString());
    REQUIRE(c.model());
    CHECK(c.model()->mesh.elementCount() == 4);
    CHECK(c.model()->mesh.nodeCount() == 9);
    CHECK_FALSE(c.model()->parameters.empty());
    CHECK(c.deck() != nullptr);
    CHECK_FALSE(c.model()->synthetic);
    CHECK(c.model()->frames.empty());  // increments are only known after the nominal Abaqus analysis
    bool hasSource = false;
    for (const auto& p : c.model()->parameters)
        if (!p.targets.empty() && p.targets.front().ref.line > 0) hasSource = true;
    CHECK(hasSource);
    RunSettings s;
    s.parameters = {0};
    s.maxOrder = 1;
    const auto est = c.estimate(s);
    CHECK(est.lines.join(" ").contains("known after the nominal Abaqus analysis"));
    CHECK(est.lines.join(" ").contains("per increment"));
}
