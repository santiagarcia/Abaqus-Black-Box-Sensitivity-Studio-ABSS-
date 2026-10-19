// Discovery on the example decks and token-level patching.
#include <doctest/doctest.h>

#include "inp_test_util.hpp"

#include <cmath>
#include <set>

using namespace abss;
using namespace abss::inp;
using namespace abss_test;

namespace {

const ParameterInfo& need(const std::vector<ParameterInfo>& ps, const char* name) {
    const ParameterInfo* p = findParam(ps, name);
    if (!p) {
        std::string all;
        for (const auto& q : ps) all += "  " + q.name + "\n";
        FAIL("parameter not found: " << name << "\navailable:\n" << all);
    }
    return *p;
}

std::size_t countCategory(const std::vector<ParameterInfo>& ps, ParamCategory c) {
    std::size_t n = 0;
    for (const auto& p : ps)
        if (p.category == c) ++n;
    return n;
}

const char* kExamples[] = {"truss.inp",      "cps4_patch.inp", "cps8_mixed.inp", "c3d8_block.inp",
                           "s4r_plate.inp",  "j2_plastic.inp", "umat_props.inp", "uel_props.inp"};

}  // namespace

TEST_CASE("all example decks parse without errors and round-trip") {
    for (const char* ex : kExamples) {
        CAPTURE(ex);
        const Deck d = Deck::load(examplePath(ex));
        CHECK(countErrors(d) == 0);
        CHECK(d.missingIncludes().empty());
        CHECK(d.mesh().nodeCount() > 0);
        CHECK(d.mesh().elementCount() > 0);
        CHECK(d.steps().size() >= 1);
        for (int i = 0; i < static_cast<int>(d.files().size()); ++i) CHECK(d.fileText(i) == readFile(d.files()[i].path));
        for (const auto& e : d.mesh().elements) CHECK(static_cast<int>(e.conn.size()) > 0);
    }
}

TEST_CASE("every discovered parameter has complete, exact provenance") {
    for (const char* ex : kExamples) {
        CAPTURE(ex);
        const Deck d = Deck::load(examplePath(ex));
        const auto ps = discoverParameters(d);
        CHECK(!ps.empty());
        std::set<std::string> ids;
        for (const auto& p : ps) {
            CAPTURE(p.name);
            CHECK(ids.insert(p.id).second);
            CHECK(!p.id.empty());
            CHECK(!p.name.empty());
            CHECK(!p.description.empty());
            CHECK(!p.association.empty());
            CHECK(!p.steps.empty() == (p.category != ParamCategory::Amplitude || p.note.empty()));
            REQUIRE(!p.targets.empty());
            CHECK(p.lowerBound <= p.nominal);
            CHECK(p.upperBound >= p.nominal);
            for (const auto& t : p.targets) {
                CHECK(t.ref.line > 0);
                CHECK(!t.ref.keyword.empty());
                bool fileFound = false;
                for (const auto& f : d.files())
                    if (f.path == t.ref.file) {
                        fileFound = true;
                        const auto fields = splitFields(f.lines[t.ref.line - 1]);
                        if (t.ref.keyword == "*PARAMETER") continue;
                        REQUIRE(t.ref.field < static_cast<int>(fields.size()));
                        CHECK(parseNumber(fields[t.ref.field]).value() == t.nominalToken);
                    }
                CHECK(fileFound);
            }
            if (p.mode == EditMode::Replace) CHECK(p.targets[0].nominalToken == p.nominal);
        }
    }
}

TEST_CASE("discovery: truss") {
    const Deck d = Deck::load(examplePath("truss.inp"));
    const auto ps = discoverParameters(d);
    const auto& E = need(ps, "STEEL: E");
    CHECK(E.nominal == 200000.0);
    CHECK(E.category == ParamCategory::Material);
    CHECK(E.confidence == Confidence::KnownSemantic);
    CHECK(E.mustBePositive);
    CHECK(E.id == "mat:STEEL:elastic:E");
    CHECK(E.association == "material STEEL");
    CHECK(E.steps == std::vector<std::string>{"LOAD"});
    REQUIRE(E.targets.size() == 1);
    CHECK(E.targets[0].ref.line == lineOf(d, 0, "200000., 0.3"));
    CHECK(E.targets[0].ref.field == 0);
    CHECK(E.targets[0].ref.keyword == "*ELASTIC");
    const auto& nu = need(ps, "STEEL: nu");
    CHECK(nu.nominal == 0.3);
    CHECK(nu.lowerBound == -1.0);
    CHECK(nu.upperBound == 0.5);
    CHECK_FALSE(nu.mustBePositive);
    CHECK(nu.targets[0].ref.field == 1);
    const auto& A = need(ps, "Section BAR: area");
    CHECK(A.nominal == 10.0);
    CHECK(A.category == ParamCategory::Section);
    CHECK(A.mustBePositive);
    const auto& F = need(ps, "CF1 at TIP (LOAD)");
    CHECK(F.nominal == 1000.0);
    CHECK(F.category == ParamCategory::Load);
    CHECK(F.targets[0].ref.line == lineOf(d, 0, "TIP, 1, 1000."));
    CHECK(F.targets[0].ref.field == 2);
    CHECK(countCategory(ps, ParamCategory::Boundary) == 0);  // only zero-valued BCs
    CHECK(countCategory(ps, ParamCategory::Geometry) == 6);  // ALLN, FIXED, TIP x (X, Y)
    const auto& g = need(ps, "Move set TIP in X");
    CHECK(g.mode == EditMode::Offset);
    CHECK(g.nominal == 0.0);
    CHECK(g.targets.size() == 1);
    CHECK(g.targets[0].nominalToken == 100.0);
    CHECK(g.note.find("topology") != std::string::npos);
    CHECK(ps.size() == 10);
    CHECK(d.mesh().spatialDim == 2);
}

TEST_CASE("discovery: CPS4 patch (named BC types, surface pressure, prescribed displacement)") {
    const Deck d = Deck::load(examplePath("cps4_patch.inp"));
    const auto bcs = d.boundaries();
    REQUIRE(bcs.size() == 4);
    CHECK(bcs[0].nodes.size() == 3);  // LEFT, XSYMM -> U1 ...
    CHECK(bcs[1].firstDof == 5);      // ... and UR2-UR3
    const auto ps = discoverParameters(d);
    CHECK(need(ps, "Section PLATE: thickness").nominal == 0.5);
    const auto& bc = need(ps, "Prescribed U1 at RIGHT (LOAD)");
    CHECK(bc.nominal == 0.01);
    CHECK(bc.category == ParamCategory::Boundary);
    CHECK(bc.description.find("3 node(s)") != std::string::npos);
    CHECK(need(ps, "Pressure P on TOPSURF (LOAD)").nominal == -5.0);
    CHECK(need(ps, "CF2 at 9 (LOAD)").nominal == 50.0);
    CHECK(d.nodeSets().at("LEFT").size() == 3);
}

TEST_CASE("discovery: mixed CPS8/CPS6 mesh") {
    const Deck d = Deck::load(examplePath("cps8_mixed.inp"));
    const Mesh& m = d.mesh();
    REQUIRE(m.elementCount() == 3);
    CHECK(m.elements[0].info.shape == ElementShape::Quad8);
    CHECK(m.elements[0].conn.size() == 8);
    CHECK(m.elements[1].info.shape == ElementShape::Tri6);
    CHECK(m.elements[2].conn.size() == 6);
    CHECK(d.elementSets().at("ALL").size() == 3);
    const auto ps = discoverParameters(d);
    CHECK(need(ps, "Section ALL: thickness").nominal == 1.0);
    CHECK(need(ps, "ALU: E").nominal == 70000.0);
}

TEST_CASE("discovery: C3D8 block in part/assembly form") {
    const Deck d = Deck::load(examplePath("c3d8_block.inp"));
    const Mesh& m = d.mesh();
    CHECK(m.nodeCount() == 27);
    CHECK(m.elementCount() == 8);
    CHECK(m.spatialDim == 3);
    REQUIRE(m.instanceNames == std::vector<std::string>{"BLOCK-1"});
    CHECK(m.coord(m.findNode({0, 1}))[0] == 100.0);  // translated instance
    CHECK(m.coord(m.findNode({0, 27}))[0] == 110.0);
    CHECK(d.nodeSets().at("CLAMP").size() == 9);
    CHECK(d.nodeSets().at("BLOCK-1.TOP").size() == 9);
    const auto bcs = d.boundaries();
    REQUIRE(bcs.size() == 1);
    CHECK(bcs[0].nodes.size() == 9);
    CHECK(bcs[0].lastDof == 6);
    const auto ps = discoverParameters(d);
    CHECK(need(ps, "ALU: density").nominal == 2.7e-9);
    CHECK(need(ps, "ALU: density").mustBePositive);
    CHECK(need(ps, "CF3 at BLOCK-1.21 (PRESS)").nominal == -20.0);
    CHECK(countCategory(ps, ParamCategory::Load) == 9);
    CHECK(countCategory(ps, ParamCategory::Section) == 0);  // 3D solid section has no thickness
    const auto& g = need(ps, "Move set BLOCK-1.TOP in Z");
    CHECK(g.targets.size() == 9);
    CHECK(g.targets[0].nominalToken == 10.0);  // part-local coordinate token
    CHECK(g.note.find("*PART") != std::string::npos);
}

TEST_CASE("discovery: S4R plate") {
    const Deck d = Deck::load(examplePath("s4r_plate.inp"));
    CHECK(d.mesh().elements[0].info.family == ElementFamily::Shell);
    CHECK(d.mesh().spatialDim == 3);
    const auto ps = discoverParameters(d);
    CHECK(need(ps, "Section PLATE: thickness").nominal == 2.0);
    CHECK(need(ps, "CM2 at 25 (BEND)").nominal == 50.0);
    CHECK(need(ps, "CF3 at TIP (BEND)").nominal == -2.0);
    CHECK(need(ps, "STEEL: E").nominal == 210000.0);
}

TEST_CASE("discovery: J2 plasticity, multi-step BCs, amplitudes") {
    const Deck d = Deck::load(examplePath("j2_plastic.inp"));
    REQUIRE(d.steps().size() == 2);
    CHECK(d.steps()[0].fixedIncrementation);
    CHECK(d.steps()[0].initialIncrement == 0.1);
    CHECK(d.steps()[1].name == "UNLOAD");
    const auto ps = discoverParameters(d);
    const auto& s0 = need(ps, "STEEL: sigma_y(eps_p=0)");
    CHECK(s0.nominal == 250.0);
    CHECK(s0.mustBePositive);
    CHECK(need(ps, "STEEL: sigma_y(eps_p=0.02)").nominal == 300.0);
    CHECK(need(ps, "STEEL: sigma_y(eps_p=0.1)").nominal == 350.0);
    const auto& load = need(ps, "Prescribed U1 at RIGHT (LOAD)");
    CHECK(load.nominal == 0.05);
    CHECK(load.steps == std::vector<std::string>{"LOAD", "UNLOAD"});  // OP=MOD keeps it active
    CHECK(load.description.find("LOADCURVE") != std::string::npos);
    const auto& unload = need(ps, "Prescribed U1 at RIGHT (UNLOAD)");
    CHECK(unload.nominal == 0.015);
    CHECK(unload.steps == std::vector<std::string>{"UNLOAD"});
    const auto& a = need(ps, "Amplitude LOADCURVE: a(t=0.5)");
    CHECK(a.nominal == 0.6);
    CHECK(a.category == ParamCategory::Amplitude);
    CHECK(a.steps == std::vector<std::string>{"LOAD"});
    CHECK(countCategory(ps, ParamCategory::Amplitude) == 3);
}

TEST_CASE("discovery: UMAT PROPS are opaque and indexed") {
    const Deck d = Deck::load(examplePath("umat_props.inp"));
    CHECK(d.usesUserMaterial());
    const auto ps = discoverParameters(d);
    const auto& p1 = need(ps, "UMAT UMAT_MAT: PROPS[1]");
    CHECK(p1.nominal == 210000.0);
    CHECK(p1.confidence == Confidence::OpaqueIndexed);
    CHECK(p1.category == ParamCategory::UserMaterial);
    CHECK(p1.id == "umat:UMAT_MAT:props:1");
    CHECK_FALSE(p1.mustBePositive);
    CHECK(p1.supported);
    CHECK(need(ps, "UMAT UMAT_MAT: PROPS[2]").nominal == 0.3);
    const auto& dv = need(ps, "UMAT_MAT: DEPVAR (number of SDVs)");
    CHECK_FALSE(dv.supported);
    CHECK(dv.nominal == 1.0);
    // No invented physical names for the opaque constants.
    CHECK(findParam(ps, "UMAT_MAT: E") == nullptr);
    CHECK(need(ps, "Prescribed U1 at X1 (PULL)").nominal == 0.001);
}

TEST_CASE("discovery: UEL PROPS and JPROPS") {
    const Deck d = Deck::load(examplePath("uel_props.inp"));
    CHECK(d.usesUserElement());
    CHECK(d.mesh().elements[0].info.family == ElementFamily::User);
    CHECK(d.mesh().spatialDim == 2);
    const auto ps = discoverParameters(d);
    const auto& p1 = need(ps, "UEL UTRUSS: PROPS[1]");
    CHECK(p1.nominal == 200000.0);
    CHECK(p1.confidence == Confidence::OpaqueIndexed);
    CHECK(p1.category == ParamCategory::UserElement);
    CHECK(need(ps, "UEL UTRUSS: PROPS[2]").nominal == 10.0);
    const auto& j1 = need(ps, "UEL UTRUSS: JPROPS[1]");
    CHECK_FALSE(j1.supported);
    CHECK(j1.nominal == 1.0);
    CHECK_THROWS_AS(editsForParameter(j1, 2.0), std::invalid_argument);
}

TEST_CASE("discovery: other material models and sections") {
    const std::string text =
        "*NODE\n1,0,0\n2,1,0\n3,1,1\n4,0,1\n"
        "*ELEMENT, TYPE=CPE4, ELSET=E\n1,1,2,3,4\n"
        "*MATERIAL, NAME=Comp\n*ELASTIC, TYPE=ENGINEERING CONSTANTS\n"
        "100., 50., 50., 0.3, 0.3, 0.2, 20., 20.,\n15., 0.\n"
        "100., 50., 50., 0.3, 0.3, 0.2, 20., 20.,\n15., 100.\n"
        "*MATERIAL, NAME=RUBBER\n*HYPERELASTIC, MOONEY-RIVLIN\n0.5, 0.1, 0.01\n"
        "*MATERIAL, NAME=OG\n*HYPERELASTIC, OGDEN, N=2\n1., 2., 0.5, -2., 0.01, 0.02\n"
        "*MATERIAL, NAME=POLY\n*HYPERELASTIC, POLYNOMIAL, N=2\n1., 2., 3., 4., 5., 0.1, 0.2\n"
        "*MATERIAL, NAME=TH\n*EXPANSION, TYPE=ORTHO\n1e-5, 2e-5, 3e-5\n*DENSITY\n7.8e-9, 20.\n7.7e-9, 200.\n"
        "*MATERIAL, NAME=JC\n*PLASTIC, HARDENING=JOHNSON COOK\n300., 400., 0.3, 1.0, 1500., 20.\n"
        "*SHELL SECTION, ELSET=SH, MATERIAL=X, COMPOSITE\n0.5, 3, Comp, 0.\n0.25, 3, Comp, 90.\n"
        "*BEAM SECTION, SECTION=RECT, ELSET=BM, MATERIAL=X\n2., 3.\n0., 0., -1.\n"
        "*STEP\n*STATIC\n*END STEP\n";
    const Deck d = Deck::fromString(text, "mats.inp");
    const auto ps = discoverParameters(d);
    CHECK(need(ps, "Comp: E1 [T=0]").nominal == 100.0);
    CHECK(need(ps, "Comp: G23 [T=0]").nominal == 15.0);
    CHECK(need(ps, "Comp: G23 [T=100]").targets[0].ref.line == 13);
    CHECK(need(ps, "Comp: nu12 [T=0]").nominal == 0.3);
    CHECK(need(ps, "RUBBER: C10").nominal == 0.5);
    CHECK(need(ps, "RUBBER: C01").nominal == 0.1);
    CHECK(need(ps, "RUBBER: D1").nominal == 0.01);
    CHECK(need(ps, "OG: mu2").nominal == 0.5);
    CHECK(need(ps, "OG: alpha2").nominal == -2.0);
    CHECK(need(ps, "OG: D2").nominal == 0.02);
    CHECK(need(ps, "POLY: C11").nominal == 4.0);
    CHECK(need(ps, "POLY: C02").nominal == 5.0);
    CHECK(need(ps, "POLY: D2").nominal == 0.2);
    CHECK(need(ps, "TH: alpha22").nominal == 2e-5);
    CHECK(need(ps, "TH: density [T=200]").nominal == 7.7e-9);
    CHECK(need(ps, "JC: A").nominal == 300.0);
    CHECK(need(ps, "JC: T_melt").nominal == 1500.0);
    CHECK(need(ps, "Section SH: ply 2 thickness").nominal == 0.25);
    CHECK(need(ps, "Section BM: RECT height b").nominal == 3.0);
}

TEST_CASE("patch: edit E, reparse, only the token changes, diff is reported") {
    const Deck d = Deck::load(examplePath("truss.inp"));
    const auto ps = discoverParameters(d);
    const auto& E = need(ps, "STEEL: E");
    const auto edits = editsForParameter(E, 210000.0);
    REQUIRE(edits.size() == 1);
    const auto out = applyEdits(d, edits);
    REQUIRE(out.size() == 1);
    const Deck e = Deck::fromString(out[0], d.files()[0].path);
    const auto ps2 = discoverParameters(e);
    CHECK(need(ps2, "STEEL: E").nominal == 210000.0);
    CHECK(need(ps2, "STEEL: nu").nominal == 0.3);
    const auto& a = d.files()[0].lines;
    const auto& b = e.files()[0].lines;
    REQUIRE(a.size() == b.size());
    int changed = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            ++changed;
            CHECK(b[i] == "2.1E+05, 0.3");
        }
    CHECK(changed == 1);
    const std::string diff = describeEdits(d, edits);
    CHECK(diff.find("-200000., 0.3") != std::string::npos);
    CHECK(diff.find("+2.1E+05, 0.3") != std::string::npos);
    CHECK(diff.find("@@ -" + std::to_string(E.targets[0].ref.line)) != std::string::npos);
}

TEST_CASE("patch: every supported parameter of every example can be perturbed and re-read") {
    for (const char* ex : kExamples) {
        const Deck d = Deck::load(examplePath(ex));
        const auto ps = discoverParameters(d);
        for (const auto& p : ps) {
            if (!p.supported) continue;
            CAPTURE(ex);
            CAPTURE(p.name);
            const double v = p.nominal + p.absoluteStep();
            const auto edits = editsForParameter(p, v);
            const auto out = applyEdits(d, edits);
            REQUIRE(out.size() == d.files().size());
            const Deck e = Deck::fromString(out[0], d.files()[0].path);
            CHECK(countErrors(e) == 0);
            std::set<int> targetLines;
            for (const auto& t : p.targets) targetLines.insert(t.ref.line);
            int changed = 0;
            for (std::size_t i = 0; i < d.files()[0].lines.size(); ++i) {
                if (d.files()[0].lines[i] != e.files()[0].lines[i]) {
                    ++changed;
                    CHECK(targetLines.count(static_cast<int>(i) + 1) == 1);
                }
            }
            CHECK(changed == static_cast<int>(targetLines.size()));
            if (p.mode == EditMode::Replace) {
                const auto ps2 = discoverParameters(e);
                const ParameterInfo* q = nullptr;
                for (const auto& x : ps2)
                    if (x.id == p.id) q = &x;
                REQUIRE(q != nullptr);
                CHECK(q->nominal == v);
            } else {
                // Geometry offset: every target coordinate moved by exactly the step.
                for (const auto& t : p.targets) {
                    const auto f = splitFields(e.files()[0].lines[t.ref.line - 1]);
                    CHECK(parseNumber(f[t.ref.field]).value() == t.nominalToken + v);
                }
            }
        }
    }
}

TEST_CASE("patch: geometry offset moves node coordinates") {
    const Deck d = Deck::load(examplePath("truss.inp"));
    const auto ps = discoverParameters(d);
    const auto out = applyEdits(d, editsForParameter(need(ps, "Move set TIP in X"), 1.5));
    const Deck e = Deck::fromString(out[0], d.files()[0].path);
    CHECK(e.mesh().coord(e.mesh().findNode({-1, 2}))[0] == 101.5);
    CHECK(e.mesh().coord(e.mesh().findNode({-1, 1}))[0] == 0.0);
    // Node-level expert API.
    const auto node1 = d.mesh().findNode({-1, 1});
    const ParameterInfo g = makeGeometryParameter(d, {node1}, 2, "node 1 y");
    CHECK(g.targets.size() == 1);
    CHECK(g.targets[0].ref.field == 2);
    const Deck f = Deck::fromString(applyEdits(d, editsForParameter(g, -0.25))[0], d.files()[0].path);
    CHECK(f.mesh().coord(f.mesh().findNode({-1, 1}))[1] == -0.25);
    CHECK_THROWS_AS(makeGeometryParameter(d, {}, 1, "x"), std::invalid_argument);
    CHECK_THROWS_AS(makeGeometryParameter(d, {node1}, 4, "x"), std::invalid_argument);
}

TEST_CASE("patch: CRLF preserved, scale/offset modes, appended field, invalid references") {
    const std::string text = "*NODE\r\n1, 2., 4.\r\n*BOUNDARY\r\n1, 1, 1\r\n";
    const Deck d = Deck::fromString(text, "crlf.inp");
    const std::string path = d.files()[0].path;
    ParameterInfo s;
    s.name = "scale";
    s.mode = EditMode::Scale;
    s.nominal = 2.0;
    s.targets = {{{path, 2, 1, "*NODE"}, 2.0, 1.0}, {{path, 2, 2, "*NODE"}, 4.0, 1.0}};
    auto out = applyEdits(d, editsForParameter(s, 3.0));
    CHECK(out[0] == "*NODE\r\n1, 3.0E+00, 6.0E+00\r\n*BOUNDARY\r\n1, 1, 1\r\n");
    s.mode = EditMode::Offset;
    s.targets[1].weight = -2.0;
    out = applyEdits(d, editsForParameter(s, 3.0));
    CHECK(out[0] == "*NODE\r\n1, 3.0E+00, 2.0E+00\r\n*BOUNDARY\r\n1, 1, 1\r\n");
    s.mode = EditMode::Scale;
    s.nominal = 0.0;
    CHECK_THROWS_AS(editsForParameter(s, 1.0), std::invalid_argument);
    // Append the missing magnitude field of a BC line.
    out = applyEdits(d, {{{path, 4, 3, "*BOUNDARY"}, 0.5}});
    CHECK(out[0] == "*NODE\r\n1, 2., 4.\r\n*BOUNDARY\r\n1, 1, 1, 5.0E-01\r\n");
    const Deck e = Deck::fromString(out[0], path);
    REQUIRE(e.boundaries().size() == 1);
    CHECK(e.boundaries()[0].magnitude == 0.5);
    // Invalid references.
    CHECK_THROWS_AS(applyEdits(d, {{{path, 1, 0, ""}, 1.0}}), std::runtime_error);       // keyword line
    CHECK_THROWS_AS(applyEdits(d, {{{path, 9, 0, ""}, 1.0}}), std::runtime_error);       // line out of range
    CHECK_THROWS_AS(applyEdits(d, {{{path, 2, 7, ""}, 1.0}}), std::runtime_error);       // field beyond end
    CHECK_THROWS_AS(applyEdits(d, {{{"other.inp", 2, 1, ""}, 1.0}}), std::runtime_error);  // unknown file
    const Deck lab = Deck::fromString("*BOUNDARY\nLEFT, 1, 1\n", "lab.inp");
    CHECK_THROWS_AS(applyEdits(lab, {{{lab.files()[0].path, 2, 0, ""}, 1.0}}), std::runtime_error);  // non-numeric
}

TEST_CASE("custom expert parameter at an exact location") {
    const Deck d = Deck::load(examplePath("truss.inp"));
    const int line = lineOf(d, 0, "TIP, 1, 1000.");
    const ParameterInfo c = makeCustomParameter(d, {d.files()[0].path, line, 2, ""}, "my load");
    CHECK(c.nominal == 1000.0);
    CHECK(c.category == ParamCategory::Custom);
    CHECK(c.confidence == Confidence::OpaqueIndexed);
    CHECK(c.association == "*CLOAD");
    const Deck e = Deck::fromString(applyEdits(d, editsForParameter(c, 1234.5))[0], d.files()[0].path);
    CHECK(need(discoverParameters(e), "CF1 at TIP (LOAD)").nominal == 1234.5);
    CHECK_THROWS_AS(makeCustomParameter(d, {d.files()[0].path, line, 0, ""}, "x"), std::invalid_argument);
    CHECK_THROWS_AS(makeCustomParameter(d, {d.files()[0].path, 1, 0, ""}, "x"), std::invalid_argument);
}

TEST_CASE("writeEditedDeck copies includes, relocates outside includes, never touches originals") {
    TempDir td("write");
    writeFile(td.file("model/root.inp"),
              "*HEADING\nwrite test\n*NODE\n1, 0., 0.\n2, 10., 0.\n*ELEMENT, TYPE=T2D2, ELSET=B\n1, 1, 2\n"
              "*SOLID SECTION, ELSET=B, MATERIAL=M\n1.\n"
              "*INCLUDE, INPUT=inc/mat.inp\n"
              "*STEP\n*STATIC\n*BOUNDARY\n1, 1, 2\n2, 2, 2\n"
              "*INCLUDE, INPUT=\"../shared stuff/load.inp\"\n*END STEP\n");
    writeFile(td.file("model/inc/mat.inp"), "*MATERIAL, NAME=M\r\n*ELASTIC\r\n100., 0.3\r\n");
    writeFile(td.file("shared stuff/load.inp"), "*CLOAD\n2, 1, 5.\n");
    const Deck d = Deck::load(td.file("model/root.inp"));
    REQUIRE(d.files().size() == 3);
    REQUIRE(d.missingIncludes().empty());
    const auto ps = discoverParameters(d);
    std::vector<TokenEdit> edits = editsForParameter(need(ps, "M: E"), 150.0);
    const auto le = editsForParameter(need(ps, "CF1 at 2 (Step-1)"), 7.5);
    edits.insert(edits.end(), le.begin(), le.end());
    const std::string before0 = readFile(d.files()[0].path), before1 = readFile(d.files()[1].path),
                      before2 = readFile(d.files()[2].path);

    const std::string root = writeEditedDeck(d, edits, td.file("out dir"), "job_1");
    CHECK(fs::exists(u8(root)));
    CHECK(u8(root).filename() == "job_1.inp");
    CHECK(fs::exists(u8(td.file("out dir/inc/mat.inp"))));
    // Originals untouched.
    CHECK(readFile(d.files()[0].path) == before0);
    CHECK(readFile(d.files()[1].path) == before1);
    CHECK(readFile(d.files()[2].path) == before2);
    // Edited include keeps CRLF and changes only the token.
    CHECK(readFile(td.file("out dir/inc/mat.inp")) == "*MATERIAL, NAME=M\r\n*ELASTIC\r\n1.5E+02, 0.3\r\n");
    // The edited deck loads, uses the copies, and has the new values.
    const Deck e = Deck::load(root);
    CHECK(e.missingIncludes().empty());
    CHECK(countErrors(e) == 0);
    for (const auto& f : e.files()) CHECK(f.path.find("out dir") != std::string::npos);
    const auto ps2 = discoverParameters(e);
    CHECK(need(ps2, "M: E").nominal == 150.0);
    CHECK(need(ps2, "CF1 at 2 (Step-1)").nominal == 7.5);
    // Only the relocated include's INPUT= line changed in the root copy.
    const auto& a = d.files()[0].lines;
    const auto& b = e.files()[0].lines;
    REQUIRE(a.size() == b.size());
    int changed = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            ++changed;
            CHECK(b[i].find("*INCLUDE, INPUT=") == 0);
            CHECK(b[i].find("abss_includes") != std::string::npos);
        }
    CHECK(changed == 1);
    // Refuse to overwrite the original deck.
    CHECK_THROWS(writeEditedDeck(d, edits, td.file("model"), "root"));
}
