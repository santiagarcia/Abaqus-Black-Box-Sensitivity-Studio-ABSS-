#include "abss/core/results.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace abss {

const char* locationName(FieldLocation l) {
    switch (l) {
        case FieldLocation::Node: return "node";
        case FieldLocation::IntegrationPoint: return "integration point";
        case FieldLocation::Element: return "element";
    }
    return "?";
}

const char* qualityName(QualityStatus q) {
    switch (q) {
        case QualityStatus::Validated: return "Validated";
        case QualityStatus::Experimental: return "Experimental";
        case QualityStatus::Unverified: return "Unverified";
        case QualityStatus::Failed: return "Failed";
        case QualityStatus::Synthetic: return "Synthetic demo";
    }
    return "?";
}

PointLayout PointLayout::uniform(std::int32_t nElem, int pointsPerElement) {
    PointLayout p;
    p.pointOffset.resize(static_cast<size_t>(nElem) + 1);
    for (std::int32_t e = 0; e <= nElem; ++e) p.pointOffset[static_cast<size_t>(e)] = e * pointsPerElement;
    p.ipNumber.resize(static_cast<size_t>(nElem) * pointsPerElement);
    p.sectionPoint.assign(p.ipNumber.size(), 0);
    for (size_t i = 0; i < p.ipNumber.size(); ++i) p.ipNumber[i] = static_cast<std::int16_t>(i % pointsPerElement + 1);
    return p;
}

bool DerivativeSpec::isPure() const {
    return std::all_of(params.begin(), params.end(), [&](int p) { return p == params.front(); });
}

int ResultStore::fieldIndex(const std::string& name) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == name) return static_cast<int>(i);
    return -1;
}

std::int64_t ResultStore::locationCount(int field) const {
    switch (fields.at(static_cast<size_t>(field)).location) {
        case FieldLocation::Node: return mesh.nodeCount();
        case FieldLocation::IntegrationPoint: return points.pointCount();
        case FieldLocation::Element: return mesh.elementCount();
    }
    return 0;
}

void ResultStore::set(int field, const DerivativeSpec& d, int frame, std::vector<double> values) {
    const auto expected = static_cast<size_t>(locationCount(field) * fields.at(static_cast<size_t>(field)).componentCount());
    if (values.size() != expected)
        throw std::invalid_argument("ResultStore::set: size mismatch for field " + fields[static_cast<size_t>(field)].name);
    auto& s = data_[{field, d}];
    if (s.frames.size() < frames.size()) s.frames.resize(frames.size());
    s.frames.at(static_cast<size_t>(frame)) = std::move(values);
}

const std::vector<double>* ResultStore::get(int field, const DerivativeSpec& d, int frame) const {
    auto it = data_.find({field, d});
    if (it == data_.end() || frame < 0 || static_cast<size_t>(frame) >= it->second.frames.size()) return nullptr;
    const auto& v = it->second.frames[static_cast<size_t>(frame)];
    return v.empty() ? nullptr : &v;
}

bool ResultStore::has(int field, const DerivativeSpec& d) const { return data_.count({field, d}) > 0; }

std::vector<DerivativeSpec> ResultStore::derivatives() const {
    std::vector<DerivativeSpec> out;
    for (const auto& [k, v] : data_)
        if (std::find(out.begin(), out.end(), k.second) == out.end()) out.push_back(k.second);
    std::sort(out.begin(), out.end(), [](const DerivativeSpec& a, const DerivativeSpec& b) {
        return a.order() != b.order() ? a.order() < b.order() : a.params < b.params;
    });
    return out;
}

std::vector<double> ResultStore::history(int field, const DerivativeSpec& d, std::int64_t location, int component) const {
    const int nc = fields.at(static_cast<size_t>(field)).componentCount();
    std::vector<double> h(frames.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t f = 0; f < frames.size(); ++f)
        if (const auto* v = get(field, d, static_cast<int>(f)))
            h[f] = (*v)[static_cast<size_t>(location * nc + component)];
    return h;
}

std::string ResultStore::derivativeLabel(const std::string& quantity, const DerivativeSpec& d) const {
    if (d.order() == 0) return quantity;
    auto pname = [&](int i) {
        return (i >= 0 && static_cast<size_t>(i) < parameters.size()) ? parameters[static_cast<size_t>(i)].name
                                                                       : "a" + std::to_string(i);
    };
    static const char* sup[] = {"", "", "²", "³", "⁴", "⁵", "⁶"};
    const int k = d.order();
    const std::string ks = k < 7 ? sup[k] : "^" + std::to_string(k);
    if (d.isPure()) {
        // Total derivative along the equilibrium/history branch.
        return std::string("d") + ks + quantity + "/d" + pname(d.params[0]) + ks;
    }
    std::string den;
    for (int p : d.params) den += "∂" + pname(p);
    return std::string("∂") + ks + quantity + "/" + den;
}

}  // namespace abss
