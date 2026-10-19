// ABSS core: element classification and mesh container.
#include "abss/core/mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace abss {

namespace {

std::string upperTrim(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool startsWith(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

/// Leading decimal digits of s starting at pos (empty if none).
std::string digitsAt(const std::string& s, std::size_t pos) {
    std::string d;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) d.push_back(s[pos++]);
    return d;
}

int topoDimOf(ElementShape s) {
    switch (s) {
        case ElementShape::Point1: return 0;
        case ElementShape::Line2:
        case ElementShape::Line3: return 1;
        case ElementShape::Tri3:
        case ElementShape::Tri6:
        case ElementShape::Quad4:
        case ElementShape::Quad8:
        case ElementShape::Quad9: return 2;
        case ElementShape::Tet4:
        case ElementShape::Tet10:
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
        case ElementShape::Hex8:
        case ElementShape::Hex20:
        case ElementShape::Hex27: return 3;
        default: return 0;
    }
}

int cornerNodesOf(ElementShape s) {
    switch (s) {
        case ElementShape::Point1: return 1;
        case ElementShape::Line2:
        case ElementShape::Line3: return 2;
        case ElementShape::Tri3:
        case ElementShape::Tri6: return 3;
        case ElementShape::Quad4:
        case ElementShape::Quad8:
        case ElementShape::Quad9: return 4;
        case ElementShape::Tet4:
        case ElementShape::Tet10: return 4;
        case ElementShape::Wedge6:
        case ElementShape::Wedge15: return 6;
        case ElementShape::Hex8:
        case ElementShape::Hex20:
        case ElementShape::Hex27: return 8;
        default: return 0;
    }
}

ElementShape planarShape(int n) {
    switch (n) {
        case 3: return ElementShape::Tri3;
        case 4: return ElementShape::Quad4;
        case 6: return ElementShape::Tri6;
        case 8: return ElementShape::Quad8;
        case 9: return ElementShape::Quad9;
        default: return ElementShape::Unknown;
    }
}

ElementShape solidShape(int n) {
    switch (n) {
        case 4: return ElementShape::Tet4;
        case 6: return ElementShape::Wedge6;
        case 8: return ElementShape::Hex8;
        case 10: return ElementShape::Tet10;
        case 15: return ElementShape::Wedge15;
        case 20: return ElementShape::Hex20;
        case 27: return ElementShape::Hex27;
        default: return ElementShape::Unknown;
    }
}

/// Best guess from the node count alone (user elements and unknown names).
ElementShape shapeFromNodeCount(int n) {
    switch (n) {
        case 1: return ElementShape::Point1;
        case 2: return ElementShape::Line2;
        case 3: return ElementShape::Tri3;
        case 4: return ElementShape::Quad4;
        case 6: return ElementShape::Tri6;
        case 8: return ElementShape::Hex8;
        case 9: return ElementShape::Quad9;
        case 10: return ElementShape::Tet10;
        case 15: return ElementShape::Wedge15;
        case 20: return ElementShape::Hex20;
        case 27: return ElementShape::Hex27;
        default: return ElementShape::Unknown;
    }
}

ElementTypeInfo make(ElementShape s, ElementFamily f) {
    ElementTypeInfo info;
    info.shape = s;
    info.family = f;
    info.topoDim = topoDimOf(s);
    info.cornerNodes = cornerNodesOf(s);
    return info;
}

}  // namespace

ElementTypeInfo classifyElementType(const std::string& abaqusType, int nodeCount) {
    const std::string t = upperTrim(abaqusType);
    if (t.empty()) return make(shapeFromNodeCount(nodeCount), ElementFamily::Unknown);

    // User elements: U1, U2, ... (also VU for Explicit).
    if ((t[0] == 'U' && t.size() > 1 && std::isdigit(static_cast<unsigned char>(t[1]))) ||
        (startsWith(t, "VU") && t.size() > 2 && std::isdigit(static_cast<unsigned char>(t[2])))) {
        return make(shapeFromNodeCount(nodeCount), ElementFamily::User);
    }

    // Trusses: T2D2, T2D3, T3D2, T3D3 (+H/T/E suffixes).
    if (startsWith(t, "T2D") || startsWith(t, "T3D")) {
        const std::string d = digitsAt(t, 3);
        return make(d == "3" ? ElementShape::Line3 : ElementShape::Line2, ElementFamily::Truss);
    }
    // Beams: B21, B22, B23, B31, B32, B33 (+H, OS), PIPE21/22/31/32, ELBOW31/32.
    if (t[0] == 'B' && t.size() >= 3 && std::isdigit(static_cast<unsigned char>(t[1])) &&
        std::isdigit(static_cast<unsigned char>(t[2]))) {
        return make(t[2] == '2' ? ElementShape::Line3 : ElementShape::Line2, ElementFamily::Beam);
    }
    if (startsWith(t, "PIPE") || startsWith(t, "ELBOW")) {
        const std::size_t p = startsWith(t, "PIPE") ? 4 : 5;
        const std::string d = digitsAt(t, p);
        return make(d.size() == 2 && d[1] == '2' ? ElementShape::Line3 : ElementShape::Line2, ElementFamily::Beam);
    }

    // Continuum 3D: C3D*, DC3D*, AC3D*, plus coupled variants; count follows the prefix.
    for (const char* pre : {"C3D", "DC3D", "AC3D", "DCC3D", "Q3D"}) {
        if (startsWith(t, pre)) {
            const std::string d = digitsAt(t, std::char_traits<char>::length(pre));
            const int n = d.empty() ? nodeCount : std::stoi(d);
            return make(solidShape(n), ElementFamily::Continuum3D);
        }
    }
    // Cohesive elements (before plane continua because COH2D4 contains '2D').
    if (startsWith(t, "COH2D") || startsWith(t, "COHAX")) {
        const std::string d = digitsAt(t, 5);
        return make(planarShape(d.empty() ? nodeCount : std::stoi(d)), ElementFamily::Other);
    }
    if (startsWith(t, "COH3D")) {
        const std::string d = digitsAt(t, 5);
        return make(solidShape(d.empty() ? nodeCount : std::stoi(d)), ElementFamily::Other);
    }
    // Axisymmetric continuum: CAX*, CGAX*, DCAX*, ACAX*.
    for (const char* pre : {"CGAX", "DCCAX", "DCAX", "ACAX", "CAX"}) {
        if (startsWith(t, pre)) {
            const std::string d = digitsAt(t, std::char_traits<char>::length(pre));
            return make(planarShape(d.empty() ? nodeCount : std::stoi(d)), ElementFamily::Axisymmetric);
        }
    }
    // Plane continuum: CPS*, CPE*, CPEG*, CGPE*, DC2D*, AC2D*, DCC2D*.
    for (const char* pre : {"CPEG", "CGPE", "CPS", "CPE", "DCC2D", "DC2D", "AC2D"}) {
        if (startsWith(t, pre)) {
            const std::string d = digitsAt(t, std::char_traits<char>::length(pre));
            return make(planarShape(d.empty() ? nodeCount : std::stoi(d)), ElementFamily::Continuum2D);
        }
    }
    // Continuum shells SC6R, SC8R (+T).
    if (startsWith(t, "SC")) {
        const std::string d = digitsAt(t, 2);
        if (!d.empty()) return make(solidShape(std::stoi(d)), ElementFamily::Shell);
    }
    // Triangular shells STRI3, STRI65.
    if (startsWith(t, "STRI")) {
        const std::string d = digitsAt(t, 4);
        return make(d == "65" ? ElementShape::Tri6 : ElementShape::Tri3, ElementFamily::Shell);
    }
    // Axisymmetric shells/membranes SAX1/SAX2, MAX1/MAX2, MGAX.
    if (startsWith(t, "SAX") || startsWith(t, "MAX") || startsWith(t, "MGAX")) {
        const std::string d = digitsAt(t, startsWith(t, "MGAX") ? 4 : 3);
        return make(d == "2" ? ElementShape::Line3 : ElementShape::Line2,
                    t[0] == 'S' ? ElementFamily::Shell : ElementFamily::Membrane);
    }
    // Membranes M3D3, M3D4(R), M3D6, M3D8(R), M3D9(R).
    if (startsWith(t, "M3D")) {
        const std::string d = digitsAt(t, 3);
        return make(planarShape(d.empty() ? nodeCount : std::stoi(d)), ElementFamily::Membrane);
    }
    // Conventional shells S3, S3R, S4, S4R, S4R5, S8R, S8R5, S9R5.
    if (t[0] == 'S' && t.size() >= 2 && std::isdigit(static_cast<unsigned char>(t[1]))) {
        const int n = t[1] - '0';
        return make(planarShape(n), ElementFamily::Shell);
    }
    // Rigid elements R2D2, R3D3, R3D4, RAX2, RB2D2, RB3D2.
    if (startsWith(t, "R2D") || startsWith(t, "R3D") || startsWith(t, "RAX") || startsWith(t, "RB2D") ||
        startsWith(t, "RB3D")) {
        const std::size_t p = (startsWith(t, "RB") ? 4 : 3);
        const std::string d = digitsAt(t, p);
        const int n = d.empty() ? nodeCount : std::stoi(d);
        return make(n == 2 ? ElementShape::Line2 : planarShape(n), ElementFamily::Other);
    }
    // Point and connector-like elements.
    if (t == "MASS" || t == "ROTARYI" || t == "SPRING1" || t == "DASHPOT1" || t == "HEATCAP")
        return make(ElementShape::Point1, ElementFamily::Other);
    if (startsWith(t, "SPRING") || startsWith(t, "DASHPOT") || startsWith(t, "CONN") || startsWith(t, "GAP") ||
        startsWith(t, "JOINT") || startsWith(t, "ITS"))
        return make(ElementShape::Line2, ElementFamily::Other);

    return make(shapeFromNodeCount(nodeCount), ElementFamily::Unknown);
}

const char* shapeName(ElementShape s) {
    switch (s) {
        case ElementShape::Point1: return "Point1";
        case ElementShape::Line2: return "Line2";
        case ElementShape::Line3: return "Line3";
        case ElementShape::Tri3: return "Tri3";
        case ElementShape::Tri6: return "Tri6";
        case ElementShape::Quad4: return "Quad4";
        case ElementShape::Quad8: return "Quad8";
        case ElementShape::Quad9: return "Quad9";
        case ElementShape::Tet4: return "Tet4";
        case ElementShape::Tet10: return "Tet10";
        case ElementShape::Wedge6: return "Wedge6";
        case ElementShape::Wedge15: return "Wedge15";
        case ElementShape::Hex8: return "Hex8";
        case ElementShape::Hex20: return "Hex20";
        case ElementShape::Hex27: return "Hex27";
        default: return "Unknown";
    }
}

int shapeNodeCount(ElementShape s) {
    switch (s) {
        case ElementShape::Point1: return 1;
        case ElementShape::Line2: return 2;
        case ElementShape::Line3: return 3;
        case ElementShape::Tri3: return 3;
        case ElementShape::Tri6: return 6;
        case ElementShape::Quad4: return 4;
        case ElementShape::Quad8: return 8;
        case ElementShape::Quad9: return 9;
        case ElementShape::Tet4: return 4;
        case ElementShape::Tet10: return 10;
        case ElementShape::Wedge6: return 6;
        case ElementShape::Wedge15: return 15;
        case ElementShape::Hex8: return 8;
        case ElementShape::Hex20: return 20;
        case ElementShape::Hex27: return 27;
        default: return 0;
    }
}

std::int32_t Mesh::addNode(NodeKey key, std::array<double, 3> x) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        // Abaqus semantics: a redefinition replaces the coordinates of the existing node.
        const std::int32_t i = it->second;
        xyz[3 * i] = x[0];
        xyz[3 * i + 1] = x[1];
        xyz[3 * i + 2] = x[2];
        return i;
    }
    // The index may be stale if nodeKeys was edited directly; rebuild lazily in findNode.
    const std::int32_t i = nodeCount();
    nodeKeys.push_back(key);
    xyz.insert(xyz.end(), x.begin(), x.end());
    index_.emplace(key, i);
    return i;
}

std::int32_t Mesh::findNode(NodeKey key) const {
    if (index_.size() != nodeKeys.size()) {
        // nodeKeys was populated directly (e.g. by deserialization); rebuild the lookup table.
        auto& idx = const_cast<std::unordered_map<NodeKey, std::int32_t, NodeKeyHash>&>(index_);
        idx.clear();
        for (std::int32_t i = 0; i < nodeCount(); ++i) idx.emplace(nodeKeys[i], i);
    }
    auto it = index_.find(key);
    return it == index_.end() ? -1 : it->second;
}

std::array<double, 6> Mesh::bounds() const {
    if (nodeKeys.empty() || xyz.size() < 3) return {0, 0, 0, 0, 0, 0};
    std::array<double, 6> b{};
    for (int d = 0; d < 3; ++d) {
        b[2 * d] = std::numeric_limits<double>::infinity();
        b[2 * d + 1] = -std::numeric_limits<double>::infinity();
    }
    const std::size_t n = std::min(nodeKeys.size(), xyz.size() / 3);
    for (std::size_t i = 0; i < n; ++i)
        for (int d = 0; d < 3; ++d) {
            b[2 * d] = std::min(b[2 * d], xyz[3 * i + d]);
            b[2 * d + 1] = std::max(b[2 * d + 1], xyz[3 * i + d]);
        }
    return b;
}

double Mesh::characteristicLength() const {
    if (nodeKeys.empty()) return 0.0;
    const auto b = bounds();
    const double dx = b[1] - b[0], dy = b[3] - b[2], dz = b[5] - b[4];
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    return diag > 0.0 && std::isfinite(diag) ? diag : 1.0;
}

}  // namespace abss
