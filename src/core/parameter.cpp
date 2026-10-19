// ABSS core: parameter helpers.
#include "abss/core/parameter.hpp"

#include <cmath>

namespace abss {

const char* categoryName(ParamCategory c) {
    switch (c) {
        case ParamCategory::Material: return "Material";
        case ParamCategory::UserMaterial: return "UMAT";
        case ParamCategory::UserElement: return "UEL";
        case ParamCategory::Load: return "Load";
        case ParamCategory::Boundary: return "BC";
        case ParamCategory::Geometry: return "Geometry";
        case ParamCategory::Section: return "Section";
        case ParamCategory::Amplitude: return "Amplitude";
        case ParamCategory::Custom: return "Custom";
    }
    return "Custom";
}

double ParameterInfo::absoluteStep() const {
    // h = relativeStep * max(|nominal|, guard). A zero nominal (e.g. a geometry offset, a zero load)
    // has no intrinsic scale: use half the finite bound interval when available, otherwise 1.
    double scale = std::abs(nominal);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        const bool finiteBounds = std::isfinite(lowerBound) && std::isfinite(upperBound) &&
                                  lowerBound > -1e299 && upperBound < 1e299 && upperBound > lowerBound;
        scale = finiteBounds ? 0.5 * (upperBound - lowerBound) : 1.0;
    }
    constexpr double tinyGuard = 1e-300;
    if (scale < tinyGuard) scale = tinyGuard;
    return std::abs(relativeStep) * scale;
}

}  // namespace abss
