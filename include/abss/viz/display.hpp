// ABSS visualization: what is drawn for a (field, component, derivative, increment) choice, how its color range is
// chosen, and how it is labelled. VTK-free; every display projection is named, never silent.
#pragma once

#include "abss/core/results.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace abss::viz {

// ---- Output choice and display projection ----------------------------------------------------------------------

/// One drawable scalar: a component of a field, or the Euclidean magnitude of a nominal vector field.
struct OutputChoice {
    int field = -1;
    int component = 0;       ///< ignored when magnitude
    bool magnitude = false;  ///< |U| of the nominal vector (never offered for derivatives: d|U|/da != |dU/da|)
    bool operator==(const OutputChoice& o) const {
        return field == o.field && component == o.component && magnitude == o.magnitude;
    }
};

/// How integration-point data becomes one value per drawn cell.
enum class IpProjection { ElementAverage, SinglePoint };

enum class DataAssoc { Point, Cell };

struct DisplaySpec {
    OutputChoice output;
    DerivativeSpec derivative;
    IpProjection projection = IpProjection::ElementAverage;
    int ipNumber = 1;  ///< 1-based Abaqus integration point (SinglePoint)
};

struct DisplayValues {
    DataAssoc assoc = DataAssoc::Point;
    std::vector<double> values;  ///< per mesh node (Point) or element (Cell); NaN = no data
    std::string projectionNote;  ///< e.g. "Display projection: element average of integration points"; empty for raw
    bool available = false;      ///< false when the series does not exist at this increment
};

/// All drawable outputs of a store (magnitude entries for vector fields come first), in menu order.
std::vector<OutputChoice> outputChoices(const ResultStore& store);
/// Short quantity name used inside derivative labels: "U2", "S11", "S_Mises", "PEEQ", "U: Magnitude".
std::string quantityName(const ResultStore& store, const OutputChoice& o);
/// Whether this output can be shown for this derivative (magnitude only for order 0; series must exist).
bool outputAvailable(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d);

/// Data association of a field when drawn.
DataAssoc associationOf(const ResultStore& store, int field);
/// Largest integration-point count of any element (0 when the store has no point layout).
int maxIntegrationPoints(const ResultStore& store);

DisplayValues extractDisplay(const ResultStore& store, const DisplaySpec& spec, int frame);

/// Location picked on the model: a mesh node index or an element index.
struct PickTarget {
    DataAssoc assoc = DataAssoc::Point;
    std::int32_t index = -1;
    bool operator==(const PickTarget& o) const { return assoc == o.assoc && index == o.index; }
};

/// Value history (one entry per frame, NaN where missing) at a picked location, using the same projection as
/// the 3D view so the chart and the colors always agree.
std::vector<double> historyAt(const ResultStore& store, const DisplaySpec& spec, const PickTarget& target);

/// "node 42" / "element 7" with Abaqus labels and the instance name when present.
std::string describeTarget(const ResultStore& store, const PickTarget& target);

/// Nominal displacement of every node at a frame as xyz triplets (zeros where U is missing); empty when no U field.
std::vector<double> nodalDisplacement(const ResultStore& store, int frame);

// ---- Color range ---------------------------------------------------------------------------------------------

enum class RangeMode { AllIncrements, CurrentIncrement, Manual };
enum class ScaleMode { Linear, Log, SymLog };

struct Range {
    double lo = std::numeric_limits<double>::quiet_NaN();
    double hi = std::numeric_limits<double>::quiet_NaN();
    bool valid() const { return lo == lo && hi == hi; }
    void include(double v) {
        if (!(v == v) || std::abs(v) == std::numeric_limits<double>::infinity()) return;
        if (!valid()) lo = hi = v;
        else {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    void include(const Range& r) {
        if (r.valid()) {
            include(r.lo);
            include(r.hi);
        }
    }
};

Range finiteRange(const std::vector<double>& v);
/// Range of the displayed values over every frame that has data.
Range rangeOverAllFrames(const ResultStore& store, const DisplaySpec& spec);

struct ResolvedRange {
    double lo = 0, hi = 1;
    bool constant = false;  ///< all values equal; range padded for display
    double constantValue = 0;
    bool empty = false;     ///< no finite data at all
};

/// Final lookup-table range: optional centering at zero (symmetric +-max|v|), degenerate-range padding.
ResolvedRange resolveRange(const Range& data, bool centered);
/// Whether log scaling is valid (all values > 0).
bool logScaleValid(const Range& r);
/// Default linear threshold of the symmetric-log scale: 1e-3 of the largest magnitude (>0).
double symlogLinearThreshold(double lo, double hi);
/// Map a value to t in [0,1] for the given scale over [lo, hi] (symlog: sign(v) log10(1 + |v|/c)).
double normalizedPosition(double v, double lo, double hi, ScaleMode scale, double linthresh);

// ---- Labels ----------------------------------------------------------------------------------------------------

/// Notation: "U2 (nominal)", "dU2/dE", "d²S_Mises/dE²". Parameter symbol disambiguated when two share a name.
std::string notationLabel(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d);
/// Units line under the legend title; never invents units.
std::string unitsLine(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d);
/// Plain-language question for the current selection (Maya §5.3).
std::string questionSentence(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d,
                             DataAssoc where);
/// "Increment 7 of 31 · Step "Load" · step time 0.35 · total time 0.35"
std::string frameLabel(const ResultStore& store, int frame);
/// Compact number formatting (%.4g style; "−" for negatives is not used to keep CSV/clipboard safe).
std::string formatValue(double v, int significant = 4);
/// Display name of a parameter, disambiguated with its description context when another parameter shares the name.
std::string parameterSymbol(const ResultStore& store, int param);

}  // namespace abss::viz
