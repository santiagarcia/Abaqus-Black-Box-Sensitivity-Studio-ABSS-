// ABSS visualization: the VTK scene of one result view (mesh, colored scalars, deformation, overlays, legend,
// pick markers, camera presets) plus grid construction and VTU export.
#pragma once

#include "abss/core/results.hpp"
#include "abss/viz/colormap.hpp"
#include "abss/viz/display.hpp"

#include <vtkSmartPointer.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

class vtkRenderer;
class vtkUnstructuredGrid;

namespace abss::viz {

/// VTK cell type for an element shape (VTK_EMPTY_CELL when it cannot be drawn).
int vtkCellTypeFor(ElementShape s);

/// Mixed-element unstructured grid; cell i == mesh element i (undrawable elements become VTK_EMPTY_CELL so indices
/// stay aligned). 2D meshes get z = 0.
vtkSmartPointer<vtkUnstructuredGrid> buildGrid(const Mesh& mesh);

/// ASCII array name for VTU export: "U", "d(U)/d(E)", "d2(S)/d(E)2".
std::string exportArrayName(const ResultStore& store, const std::string& quantity, const DerivativeSpec& d);

/// Write one increment as .vtu: undeformed geometry; point arrays for nodal fields (nominal + every derivative
/// stored at this increment); cell arrays for element fields and, for integration-point fields, the element
/// average (array name suffix " [element average of IPs]"). Field data records the synthetic flag. Throws on error.
void writeFrameVtu(const ResultStore& store, int frame, const std::string& path);

class Scene {
public:
    Scene();
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    vtkRenderer* renderer() const;
    vtkUnstructuredGrid* grid() const;

    /// Rebuild geometry for a mesh (resets scalars, displacement and picks).
    void setMesh(const Mesh& mesh);
    bool hasMesh() const;
    bool isPlanar() const;  ///< all z equal (2D model)

    /// Swap the colored scalars (fast path: reuses the array). Empty values = uniform surface color.
    void setScalars(const DisplayValues& v);
    void clearScalars();
    /// Lookup table from a colormap over [lo, hi] with the given scale; NaN color for "no data".
    void setColoring(const Colormap& map, double lo, double hi, ScaleMode scale, Rgb nanColor);
    /// Legend title (derivative notation) and footer lines (units, projection note, synthetic marker).
    void setLegend(const std::string& title, const std::string& footer, bool visible);
    void setWatermark(const std::string& text);

    /// Per-node xyz displacement (3 * nodes) times scale; empty vector = undeformed.
    void setDisplacement(const std::vector<double>& d, double scale);
    void setShowUndeformed(bool on);
    void setEdges(bool on);
    void setWireframe(bool on);
    void setOpacity(double alpha);
    void setTheme(bool dark);

    void setPickMarkers(const std::vector<std::array<double, 3>>& positions, const std::vector<std::string>& labels,
                        const std::vector<Rgb>& colors);
    std::array<double, 3> nodePosition(std::int32_t node) const;
    std::array<double, 3> cellCenter(std::int32_t cell) const;

    enum class View { Fit, Iso, XY, XZ, YZ };
    void applyView(View v);
    void setParallelProjection(bool on);
    bool parallelProjection() const;

    struct Pick {
        bool hit = false;
        std::int32_t cell = -1;
        std::int32_t node = -1;  ///< nearest node of the picked cell
        std::array<double, 3> position{};
    };
    /// Pick at display coordinates (VTK convention: origin bottom-left, physical pixels). Needs a rendered window.
    Pick pick(int x, int y);
    /// Nearest node of `cell` to a world position (deformed coordinates).
    std::int32_t nearestNodeOfCell(std::int32_t cell, const std::array<double, 3>& p) const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace abss::viz
