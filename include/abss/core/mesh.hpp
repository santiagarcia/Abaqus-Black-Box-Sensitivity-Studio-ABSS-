// ABSS core: mesh model with stable Abaqus identities.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace abss {

/// Geometric shape family of an element, independent of the Abaqus formulation.
enum class ElementShape : std::uint8_t {
    Unknown,
    Point1,
    Line2,
    Line3,
    Tri3,
    Tri6,
    Quad4,
    Quad8,
    Quad9,
    Tet4,
    Tet10,
    Wedge6,
    Wedge15,
    Hex8,
    Hex20,
    Hex27,
};

/// Broad element family; used for display decisions (shells get thickness/section points).
enum class ElementFamily : std::uint8_t { Unknown, Truss, Beam, Continuum2D, Axisymmetric, Continuum3D, Shell, Membrane, User, Other };

struct ElementTypeInfo {
    ElementShape shape = ElementShape::Unknown;
    ElementFamily family = ElementFamily::Unknown;
    int topoDim = 0;    ///< 1 line, 2 surface, 3 volume
    int cornerNodes = 0;
};

/// Classify an Abaqus element type name (e.g. "CPS4R", "C3D20R", "S4R", "T2D2").
/// `nodeCount` disambiguates user elements (U1...) and unknown names.
ElementTypeInfo classifyElementType(const std::string& abaqusType, int nodeCount);

const char* shapeName(ElementShape s);
int shapeNodeCount(ElementShape s);

/// Node identity: (instance index, Abaqus label). Instance -1 = assembly level / flat deck.
struct NodeKey {
    std::int32_t instance = -1;
    std::int64_t label = 0;
    bool operator==(const NodeKey& o) const { return instance == o.instance && label == o.label; }
};

struct NodeKeyHash {
    std::size_t operator()(const NodeKey& k) const noexcept {
        return std::hash<std::int64_t>()(k.label * 1315423911LL + k.instance);
    }
};

struct Element {
    std::int64_t label = 0;
    std::int32_t instance = -1;
    std::string type;                    ///< Abaqus type, upper case
    ElementTypeInfo info;
    std::vector<std::int32_t> conn;      ///< 0-based indices into Mesh nodes
    std::int32_t integrationPoints = 0;  ///< number of material points if known (0 = unknown)
};

/// Unstructured mixed-element mesh with Abaqus labels preserved for every entity.
class Mesh {
public:
    std::string name;
    int spatialDim = 3;
    std::vector<std::string> instanceNames;

    std::vector<NodeKey> nodeKeys;
    std::vector<double> xyz;  ///< 3 * nodeCount(); z = 0 for 2D models
    std::vector<Element> elements;

    std::int32_t nodeCount() const { return static_cast<std::int32_t>(nodeKeys.size()); }
    std::int32_t elementCount() const { return static_cast<std::int32_t>(elements.size()); }

    std::int32_t addNode(NodeKey key, std::array<double, 3> x);
    /// Returns -1 when the node does not exist.
    std::int32_t findNode(NodeKey key) const;
    std::array<double, 3> coord(std::int32_t i) const { return {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}; }

    /// Axis-aligned bounds {xmin,xmax,ymin,ymax,zmin,zmax}; zeros for empty mesh.
    std::array<double, 6> bounds() const;
    double characteristicLength() const;

private:
    std::unordered_map<NodeKey, std::int32_t, NodeKeyHash> index_;
};

}  // namespace abss
