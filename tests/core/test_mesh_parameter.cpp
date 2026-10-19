// Tests for abss-core mesh classification/container and parameter helpers.
#include <doctest/doctest.h>

#include "abss/core/mesh.hpp"
#include "abss/core/parameter.hpp"

#include <cmath>
#include <string>

using namespace abss;

namespace {
void checkType(const std::string& t, ElementShape s, ElementFamily f, int topo, int nodes = 0) {
    CAPTURE(t);
    const ElementTypeInfo i = classifyElementType(t, nodes);
    CHECK(i.shape == s);
    CHECK(i.family == f);
    CHECK(i.topoDim == topo);
}
}  // namespace

TEST_CASE("classifyElementType: trusses and beams") {
    checkType("T2D2", ElementShape::Line2, ElementFamily::Truss, 1);
    checkType("t3d2", ElementShape::Line2, ElementFamily::Truss, 1);
    checkType("T3D3H", ElementShape::Line3, ElementFamily::Truss, 1);
    checkType("B21", ElementShape::Line2, ElementFamily::Beam, 1);
    checkType("B22", ElementShape::Line3, ElementFamily::Beam, 1);
    checkType("B31", ElementShape::Line2, ElementFamily::Beam, 1);
    checkType("B32", ElementShape::Line3, ElementFamily::Beam, 1);
    checkType("B33", ElementShape::Line2, ElementFamily::Beam, 1);
    checkType("B31OS", ElementShape::Line2, ElementFamily::Beam, 1);
    checkType("PIPE32", ElementShape::Line3, ElementFamily::Beam, 1);
}

TEST_CASE("classifyElementType: plane, axisymmetric and solid continua") {
    checkType("CPS3", ElementShape::Tri3, ElementFamily::Continuum2D, 2);
    checkType("CPS4", ElementShape::Quad4, ElementFamily::Continuum2D, 2);
    checkType("CPS4R", ElementShape::Quad4, ElementFamily::Continuum2D, 2);
    checkType("CPS6", ElementShape::Tri6, ElementFamily::Continuum2D, 2);
    checkType("CPS8", ElementShape::Quad8, ElementFamily::Continuum2D, 2);
    checkType("CPS8R", ElementShape::Quad8, ElementFamily::Continuum2D, 2);
    checkType("CPE4H", ElementShape::Quad4, ElementFamily::Continuum2D, 2);
    checkType("CPE8RH", ElementShape::Quad8, ElementFamily::Continuum2D, 2);
    checkType("CPEG4", ElementShape::Quad4, ElementFamily::Continuum2D, 2);
    checkType("CPE4I", ElementShape::Quad4, ElementFamily::Continuum2D, 2);
    checkType("CAX4R", ElementShape::Quad4, ElementFamily::Axisymmetric, 2);
    checkType("CAX6", ElementShape::Tri6, ElementFamily::Axisymmetric, 2);
    checkType("CAX8", ElementShape::Quad8, ElementFamily::Axisymmetric, 2);
    checkType("C3D4", ElementShape::Tet4, ElementFamily::Continuum3D, 3);
    checkType("C3D6", ElementShape::Wedge6, ElementFamily::Continuum3D, 3);
    checkType("C3D8", ElementShape::Hex8, ElementFamily::Continuum3D, 3);
    checkType("C3D8R", ElementShape::Hex8, ElementFamily::Continuum3D, 3);
    checkType("C3D8I", ElementShape::Hex8, ElementFamily::Continuum3D, 3);
    checkType("C3D8H", ElementShape::Hex8, ElementFamily::Continuum3D, 3);
    checkType("C3D10", ElementShape::Tet10, ElementFamily::Continuum3D, 3);
    checkType("C3D10M", ElementShape::Tet10, ElementFamily::Continuum3D, 3);
    checkType("C3D15", ElementShape::Wedge15, ElementFamily::Continuum3D, 3);
    checkType("C3D20", ElementShape::Hex20, ElementFamily::Continuum3D, 3);
    checkType("C3D20R", ElementShape::Hex20, ElementFamily::Continuum3D, 3);
    checkType("C3D20RH", ElementShape::Hex20, ElementFamily::Continuum3D, 3);
    checkType("DC3D8", ElementShape::Hex8, ElementFamily::Continuum3D, 3);
}

TEST_CASE("classifyElementType: shells, membranes, user and others") {
    checkType("S3", ElementShape::Tri3, ElementFamily::Shell, 2);
    checkType("S3R", ElementShape::Tri3, ElementFamily::Shell, 2);
    checkType("S4", ElementShape::Quad4, ElementFamily::Shell, 2);
    checkType("S4R", ElementShape::Quad4, ElementFamily::Shell, 2);
    checkType("S4R5", ElementShape::Quad4, ElementFamily::Shell, 2);
    checkType("S8R", ElementShape::Quad8, ElementFamily::Shell, 2);
    checkType("S8R5", ElementShape::Quad8, ElementFamily::Shell, 2);
    checkType("S9R5", ElementShape::Quad9, ElementFamily::Shell, 2);
    checkType("STRI3", ElementShape::Tri3, ElementFamily::Shell, 2);
    checkType("STRI65", ElementShape::Tri6, ElementFamily::Shell, 2);
    checkType("SC8R", ElementShape::Hex8, ElementFamily::Shell, 3);
    checkType("SC6R", ElementShape::Wedge6, ElementFamily::Shell, 3);
    checkType("M3D3", ElementShape::Tri3, ElementFamily::Membrane, 2);
    checkType("M3D4R", ElementShape::Quad4, ElementFamily::Membrane, 2);
    checkType("M3D8", ElementShape::Quad8, ElementFamily::Membrane, 2);
    checkType("M3D9R", ElementShape::Quad9, ElementFamily::Membrane, 2);
    checkType("U1", ElementShape::Line2, ElementFamily::User, 1, 2);
    checkType("U12", ElementShape::Quad4, ElementFamily::User, 2, 4);
    checkType("U3", ElementShape::Hex20, ElementFamily::User, 3, 20);
    checkType("MASS", ElementShape::Point1, ElementFamily::Other, 0);
    checkType("CONN3D2", ElementShape::Line2, ElementFamily::Other, 1);
    checkType("R3D4", ElementShape::Quad4, ElementFamily::Other, 2);
    checkType("COH3D8", ElementShape::Hex8, ElementFamily::Other, 3);
    checkType("NOTANELEMENT", ElementShape::Hex8, ElementFamily::Unknown, 3, 8);
    CHECK(classifyElementType("C3D20R", 0).cornerNodes == 8);
    CHECK(classifyElementType("CPS6", 0).cornerNodes == 3);
}

TEST_CASE("shape names and node counts") {
    CHECK(std::string(shapeName(ElementShape::Hex20)) == "Hex20");
    CHECK(std::string(shapeName(ElementShape::Unknown)) == "Unknown");
    CHECK(shapeNodeCount(ElementShape::Hex20) == 20);
    CHECK(shapeNodeCount(ElementShape::Tet10) == 10);
    CHECK(shapeNodeCount(ElementShape::Wedge15) == 15);
    CHECK(shapeNodeCount(ElementShape::Quad9) == 9);
    CHECK(shapeNodeCount(ElementShape::Line3) == 3);
    CHECK(shapeNodeCount(ElementShape::Unknown) == 0);
}

TEST_CASE("Mesh: add, find, redefine, bounds, characteristic length") {
    Mesh m;
    CHECK(m.bounds() == std::array<double, 6>{0, 0, 0, 0, 0, 0});
    CHECK(m.characteristicLength() == 0.0);
    const auto a = m.addNode({-1, 1}, {0, 0, 0});
    const auto b = m.addNode({-1, 2}, {3, 4, 0});
    const auto c = m.addNode({0, 1}, {1, 1, 12});  // same label, other instance: distinct node
    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(c == 2);
    CHECK(m.findNode({-1, 2}) == 1);
    CHECK(m.findNode({0, 1}) == 2);
    CHECK(m.findNode({5, 1}) == -1);
    // Redefinition replaces coordinates, keeps index.
    CHECK(m.addNode({-1, 2}, {3, 4, 0.5}) == 1);
    CHECK(m.nodeCount() == 3);
    CHECK(m.coord(1)[2] == 0.5);
    const auto bb = m.bounds();
    CHECK(bb[0] == 0.0);
    CHECK(bb[1] == 3.0);
    CHECK(bb[3] == 4.0);
    CHECK(bb[5] == 12.0);
    CHECK(m.characteristicLength() == doctest::Approx(13.0));
    // A single node has no extent: guard length 1.
    Mesh one;
    one.addNode({-1, 7}, {2, 2, 2});
    CHECK(one.characteristicLength() == 1.0);
}

TEST_CASE("Mesh: findNode works after direct population of nodeKeys") {
    Mesh m;
    m.nodeKeys = {{-1, 10}, {-1, 20}};
    m.xyz = {0, 0, 0, 1, 0, 0};
    CHECK(m.findNode({-1, 20}) == 1);
}

TEST_CASE("categoryName and absoluteStep") {
    CHECK(std::string(categoryName(ParamCategory::Material)) == "Material");
    CHECK(std::string(categoryName(ParamCategory::UserMaterial)) == "UMAT");
    CHECK(std::string(categoryName(ParamCategory::UserElement)) == "UEL");
    CHECK(std::string(categoryName(ParamCategory::Boundary)) == "BC");
    CHECK(std::string(categoryName(ParamCategory::Geometry)) == "Geometry");

    ParameterInfo p;
    p.nominal = 200000.0;
    p.relativeStep = 1e-3;
    CHECK(p.absoluteStep() == doctest::Approx(200.0));
    p.nominal = -4.0;
    CHECK(p.absoluteStep() == doctest::Approx(4e-3));
    p.nominal = 0.0;  // no bounds: unit scale
    CHECK(p.absoluteStep() == doctest::Approx(1e-3));
    p.lowerBound = -5.0;  // finite bounds give the scale for a zero nominal
    p.upperBound = 5.0;
    CHECK(p.absoluteStep() == doctest::Approx(5e-3));
    p.nominal = 1e-20;  // tiny but nonzero nominal stays relative
    CHECK(p.absoluteStep() == doctest::Approx(1e-23).epsilon(1e-6));
    CHECK(p.absoluteStep() > 0.0);
}
