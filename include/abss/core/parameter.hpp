// ABSS core: typed, provenance-carrying model parameters.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace abss {

enum class ParamCategory : std::uint8_t {
    Material,
    UserMaterial,  ///< *USER MATERIAL PROPS (opaque semantics)
    UserElement,   ///< *UEL PROPERTY PROPS (opaque semantics)
    Load,
    Boundary,
    Geometry,
    Section,
    Amplitude,
    Custom,
};
const char* categoryName(ParamCategory c);

enum class Confidence : std::uint8_t {
    KnownSemantic,  ///< e.g. *ELASTIC E: meaning is defined by Abaqus documentation
    OpaqueIndexed,  ///< e.g. UMAT PROPS[3]: only position is known
};

/// Exact location of one numeric token in the INP source tree.
struct SourceRef {
    std::string file;       ///< absolute path of the file containing the token
    int line = 0;           ///< 1-based physical line
    int field = 0;          ///< 0-based comma-separated field index on that line
    std::string keyword;    ///< owning keyword, upper case, e.g. "*ELASTIC"
};

/// How a perturbation of the scalar maps onto INP tokens.
enum class EditMode : std::uint8_t {
    Replace,  ///< every target token := value
    Scale,    ///< every target token := nominal_token * (value / nominal)
    Offset,   ///< every target token := nominal_token + (value - nominal) * weight
};

struct ParameterTarget {
    SourceRef ref;
    double nominalToken = 0.0;
    double weight = 1.0;
};

/// One scalar design parameter a; may drive one token or a group (e.g. a shape mode moving many nodes).
struct ParameterInfo {
    std::string id;           ///< stable key, e.g. "mat:STEEL:elastic:E"
    std::string name;         ///< editable display name, e.g. "E"
    std::string description;  ///< plain-language description
    ParamCategory category = ParamCategory::Custom;
    Confidence confidence = Confidence::KnownSemantic;
    double nominal = 0.0;
    std::string units;        ///< only if supplied by the user/deck; Abaqus is unit-agnostic
    bool mustBePositive = false;
    double lowerBound = -1e300;
    double upperBound = 1e300;
    double relativeStep = 1e-3; ///< default perturbation h / |a|
    std::string association;  ///< material/elset/nset/node/step context
    std::vector<std::string> steps;
    EditMode mode = EditMode::Replace;
    std::vector<ParameterTarget> targets;
    bool supported = true;    ///< false = shown but cannot be differentiated (reason in note)
    std::string note;

    /// Absolute perturbation step for this parameter.
    double absoluteStep() const;
};

}  // namespace abss
