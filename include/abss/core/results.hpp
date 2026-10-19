// ABSS core: field/derivative result model shared by solver, persistence and GUI.
#pragma once

#include "abss/core/mesh.hpp"
#include "abss/core/parameter.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace abss {

enum class FieldLocation : std::uint8_t { Node, IntegrationPoint, Element };
enum class FieldKind : std::uint8_t { Scalar, Vector, SymTensor, Generic };

const char* locationName(FieldLocation l);

struct FieldDescriptor {
    std::string name;                     ///< "U", "RF", "S", "E", "PEEQ", "SDV" ...
    FieldLocation location = FieldLocation::Node;
    FieldKind kind = FieldKind::Scalar;
    std::vector<std::string> components;  ///< "U1","U2" / "S11","S22","S33","S12" / "PEEQ"
    std::string description;
    std::string units;
    int componentCount() const { return static_cast<int>(components.size()); }
};

/// Integration/section point layout for element-located data.
/// Material point p of element e lives at index pointOffset[e] + p (p < pointOffset[e+1]-pointOffset[e]).
/// Values are never averaged in storage; nodal projection is a display operation.
struct PointLayout {
    std::vector<std::int32_t> pointOffset;  ///< size nElem + 1
    std::vector<std::int16_t> ipNumber;     ///< 1-based Abaqus integration point per material point
    std::vector<std::int16_t> sectionPoint; ///< 1-based section point (0 = none)
    std::int32_t pointCount() const { return pointOffset.empty() ? 0 : pointOffset.back(); }
    static PointLayout uniform(std::int32_t nElem, int pointsPerElement);
};

struct FrameInfo {
    int step = 1;            ///< 1-based step number
    std::string stepName;
    int increment = 0;       ///< Abaqus increment number (0 = initial state)
    double stepTime = 0.0;
    double totalTime = 0.0;
};

/// Pure derivative of order k with respect to one parameter, mixed derivative w.r.t. a multi-index,
/// or the nominal value (order 0). Indices refer to ResultStore::parameters.
struct DerivativeSpec {
    std::vector<int> params;  ///< empty = nominal; {j,j} = d2/daj2; {j,l} = mixed
    int order() const { return static_cast<int>(params.size()); }
    bool isPure() const;
    bool operator<(const DerivativeSpec& o) const { return params < o.params; }
    bool operator==(const DerivativeSpec& o) const { return params == o.params; }
    static DerivativeSpec nominal() { return {}; }
    static DerivativeSpec pure(int param, int order) { return {std::vector<int>(static_cast<size_t>(order), param)}; }
};

enum class QualityStatus : std::uint8_t { Validated, Experimental, Unverified, Failed, Synthetic };
const char* qualityName(QualityStatus q);

/// Evidence attached to one derivative series. Every number here is measured, never assumed.
struct QualityRecord {
    QualityStatus status = QualityStatus::Unverified;
    double fdRelativeError = -1.0;        ///< max rel. error vs full-rerun finite differences (-1 = not run)
    double predictorResidualRatio = -1.0; ///< ||r(predictor, h)|| / ||r(predictor, h/2)||; ~2^(k+1) when smooth
    double tangentCheckError = -1.0;      ///< ||K v + dr/du v|| / ||K v|| (-1 = not run)
    double stepSize = 0.0;
    std::vector<int> suspectFrames;       ///< increments flagged as possibly non-smooth
    std::string scheme;
    std::vector<std::string> notes;
};

/// Complete in-memory result set: mesh, time grid, fields and every computed derivative series.
class ResultStore {
public:
    std::string title;
    std::string sourceInp;
    std::string backend;          ///< "synthetic-demo", "abaqus-2024", ...
    bool synthetic = false;
    Mesh mesh;
    PointLayout points;
    std::vector<FrameInfo> frames;
    std::vector<FieldDescriptor> fields;
    std::vector<ParameterInfo> parameters;
    std::map<DerivativeSpec, QualityRecord> quality;
    std::vector<std::string> log;

    int fieldIndex(const std::string& name) const;
    std::int64_t locationCount(int field) const;

    /// Store values (locationCount * componentCount) for (field, derivative, frame).
    void set(int field, const DerivativeSpec& d, int frame, std::vector<double> values);
    const std::vector<double>* get(int field, const DerivativeSpec& d, int frame) const;
    bool has(int field, const DerivativeSpec& d) const;
    std::vector<DerivativeSpec> derivatives() const;

    /// History of one component at one location across all frames (NaN when missing).
    std::vector<double> history(int field, const DerivativeSpec& d, std::int64_t location, int component) const;

    /// Human label, e.g. "U2", "dU2/dE", "d²S_Mises/dE²", "∂²U1/∂E∂nu".
    std::string derivativeLabel(const std::string& quantity, const DerivativeSpec& d) const;

    /// Visit every stored (field, derivative, frame, values) series entry.
    template <class F>
    void forEachSeries(F&& f) const {
        for (const auto& [key, s] : data_)
            for (size_t fr = 0; fr < s.frames.size(); ++fr)
                if (!s.frames[fr].empty()) f(key.first, key.second, static_cast<int>(fr), s.frames[fr]);
    }
    void clearSeries() { data_.clear(); }

private:
    struct Series {
        std::vector<std::vector<double>> frames;
    };
    std::map<std::pair<int, DerivativeSpec>, Series> data_;
};

}  // namespace abss
