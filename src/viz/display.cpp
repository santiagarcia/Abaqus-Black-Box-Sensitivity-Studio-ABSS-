#include "abss/viz/display.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace abss::viz {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const FieldDescriptor* fieldOf(const ResultStore& s, int f) {
    return (f >= 0 && static_cast<size_t>(f) < s.fields.size()) ? &s.fields[static_cast<size_t>(f)] : nullptr;
}

double pick(const std::vector<double>& v, std::int64_t loc, int nc, const OutputChoice& o) {
    const size_t base = static_cast<size_t>(loc) * static_cast<size_t>(nc);
    if (o.magnitude) {
        double s = 0;
        for (int c = 0; c < nc; ++c) s += v[base + static_cast<size_t>(c)] * v[base + static_cast<size_t>(c)];
        return std::sqrt(s);
    }
    return v[base + static_cast<size_t>(o.component)];
}

std::string superscript(int k) {
    static const char* sup[] = {"", "", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    return k >= 0 && k < 10 ? sup[k] : "^" + std::to_string(k);
}
}  // namespace

std::vector<OutputChoice> outputChoices(const ResultStore& store) {
    std::vector<OutputChoice> out;
    for (size_t f = 0; f < store.fields.size(); ++f) {
        const auto& fd = store.fields[f];
        if (fd.kind == FieldKind::Vector && fd.componentCount() > 1) out.push_back({static_cast<int>(f), 0, true});
        for (int c = 0; c < fd.componentCount(); ++c) out.push_back({static_cast<int>(f), c, false});
    }
    return out;
}

std::string quantityName(const ResultStore& store, const OutputChoice& o) {
    const auto* fd = fieldOf(store, o.field);
    if (!fd) return "?";
    if (o.magnitude) return fd->name + ": Magnitude";
    if (fd->componentCount() == 1) return fd->name;
    return fd->components.at(static_cast<size_t>(o.component));
}

bool outputAvailable(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d) {
    if (!fieldOf(store, o.field)) return false;
    if (o.magnitude && d.order() > 0) return false;
    return store.has(o.field, d);
}

DataAssoc associationOf(const ResultStore& store, int field) {
    const auto* fd = fieldOf(store, field);
    return fd && fd->location == FieldLocation::Node ? DataAssoc::Point : DataAssoc::Cell;
}

int maxIntegrationPoints(const ResultStore& store) {
    int m = 0;
    const auto& po = store.points.pointOffset;
    for (size_t e = 0; e + 1 < po.size(); ++e) m = std::max(m, static_cast<int>(po[e + 1] - po[e]));
    return m;
}

DisplayValues extractDisplay(const ResultStore& store, const DisplaySpec& spec, int frame) {
    DisplayValues out;
    const auto* fd = fieldOf(store, spec.output.field);
    if (!fd) return out;
    out.assoc = associationOf(store, spec.output.field);
    const int nc = fd->componentCount();
    const auto* v = (spec.output.magnitude && spec.derivative.order() > 0) ? nullptr
                                                                          : store.get(spec.output.field, spec.derivative, frame);
    const std::int64_t nOut = out.assoc == DataAssoc::Point ? store.mesh.nodeCount() : store.mesh.elementCount();
    out.values.assign(static_cast<size_t>(nOut), kNaN);
    if (fd->location == FieldLocation::IntegrationPoint) {
        if (spec.projection == IpProjection::ElementAverage)
            out.projectionNote = "Display projection: element average of integration points (stored data keeps each point)";
        else
            out.projectionNote = "Display projection: integration point " + std::to_string(spec.ipNumber) + " only";
    }
    if (!v) return out;
    out.available = true;
    switch (fd->location) {
        case FieldLocation::Node:
        case FieldLocation::Element:
            for (std::int64_t i = 0; i < nOut; ++i) out.values[static_cast<size_t>(i)] = pick(*v, i, nc, spec.output);
            break;
        case FieldLocation::IntegrationPoint: {
            const auto& po = store.points.pointOffset;
            for (std::int64_t e = 0; e < nOut && static_cast<size_t>(e) + 1 < po.size(); ++e) {
                const auto p0 = po[static_cast<size_t>(e)], p1 = po[static_cast<size_t>(e) + 1];
                if (spec.projection == IpProjection::ElementAverage) {
                    double s = 0;
                    int n = 0;
                    for (auto p = p0; p < p1; ++p) {
                        const double x = pick(*v, p, nc, spec.output);
                        if (x == x) {
                            s += x;
                            ++n;
                        }
                    }
                    if (n > 0) out.values[static_cast<size_t>(e)] = s / n;
                } else {
                    for (auto p = p0; p < p1; ++p) {
                        const bool match = static_cast<size_t>(p) < store.points.ipNumber.size()
                                               ? store.points.ipNumber[static_cast<size_t>(p)] == spec.ipNumber
                                               : (p - p0 + 1) == spec.ipNumber;
                        if (match) {
                            out.values[static_cast<size_t>(e)] = pick(*v, p, nc, spec.output);
                            break;
                        }
                    }
                }
            }
            break;
        }
    }
    return out;
}

std::vector<double> historyAt(const ResultStore& store, const DisplaySpec& spec, const PickTarget& target) {
    std::vector<double> h(store.frames.size(), kNaN);
    if (target.index < 0 || associationOf(store, spec.output.field) != target.assoc) return h;
    for (size_t f = 0; f < store.frames.size(); ++f) {
        const auto* fd = fieldOf(store, spec.output.field);
        if (!fd) break;
        const auto* v = (spec.output.magnitude && spec.derivative.order() > 0)
                            ? nullptr
                            : store.get(spec.output.field, spec.derivative, static_cast<int>(f));
        if (!v) continue;
        const int nc = fd->componentCount();
        if (fd->location != FieldLocation::IntegrationPoint) {
            h[f] = pick(*v, target.index, nc, spec.output);
            continue;
        }
        const auto& po = store.points.pointOffset;
        if (static_cast<size_t>(target.index) + 1 >= po.size()) continue;
        const auto p0 = po[static_cast<size_t>(target.index)], p1 = po[static_cast<size_t>(target.index) + 1];
        if (spec.projection == IpProjection::ElementAverage) {
            double s = 0;
            int n = 0;
            for (auto p = p0; p < p1; ++p) {
                const double x = pick(*v, p, nc, spec.output);
                if (x == x) {
                    s += x;
                    ++n;
                }
            }
            if (n > 0) h[f] = s / n;
        } else {
            for (auto p = p0; p < p1; ++p) {
                const bool match = static_cast<size_t>(p) < store.points.ipNumber.size()
                                       ? store.points.ipNumber[static_cast<size_t>(p)] == spec.ipNumber
                                       : (p - p0 + 1) == spec.ipNumber;
                if (match) {
                    h[f] = pick(*v, p, nc, spec.output);
                    break;
                }
            }
        }
    }
    return h;
}

std::string describeTarget(const ResultStore& store, const PickTarget& t) {
    const auto& m = store.mesh;
    auto inst = [&](std::int32_t i) -> std::string {
        if (i >= 0 && static_cast<size_t>(i) < m.instanceNames.size()) return " (" + m.instanceNames[static_cast<size_t>(i)] + ")";
        return "";
    };
    if (t.assoc == DataAssoc::Point) {
        if (t.index < 0 || t.index >= m.nodeCount()) return "no node";
        const auto& k = m.nodeKeys[static_cast<size_t>(t.index)];
        return "node " + std::to_string(k.label) + inst(k.instance);
    }
    if (t.index < 0 || t.index >= m.elementCount()) return "no element";
    const auto& e = m.elements[static_cast<size_t>(t.index)];
    return "element " + std::to_string(e.label) + inst(e.instance);
}

std::vector<double> nodalDisplacement(const ResultStore& store, int frame) {
    const int f = store.fieldIndex("U");
    if (f < 0 || store.fields[static_cast<size_t>(f)].location != FieldLocation::Node) return {};
    const auto& fd = store.fields[static_cast<size_t>(f)];
    std::vector<double> d(static_cast<size_t>(store.mesh.nodeCount()) * 3, 0.0);
    const auto* v = store.get(f, DerivativeSpec::nominal(), frame);
    if (!v) return d;
    const int nc = fd.componentCount();
    for (int c = 0; c < nc; ++c) {
        int axis = c;
        const auto& name = fd.components[static_cast<size_t>(c)];
        if (!name.empty() && name.back() >= '1' && name.back() <= '3') axis = name.back() - '1';
        if (axis > 2) continue;
        for (std::int32_t n = 0; n < store.mesh.nodeCount(); ++n)
            d[static_cast<size_t>(n) * 3 + static_cast<size_t>(axis)] = (*v)[static_cast<size_t>(n) * static_cast<size_t>(nc) + static_cast<size_t>(c)];
    }
    return d;
}

// ---- ranges ----------------------------------------------------------------------------------------------------

Range finiteRange(const std::vector<double>& v) {
    Range r;
    for (double x : v) r.include(x);
    return r;
}

Range rangeOverAllFrames(const ResultStore& store, const DisplaySpec& spec) {
    Range r;
    for (size_t f = 0; f < store.frames.size(); ++f) {
        const auto dv = extractDisplay(store, spec, static_cast<int>(f));
        if (dv.available) r.include(finiteRange(dv.values));
    }
    return r;
}

ResolvedRange resolveRange(const Range& data, bool centered) {
    ResolvedRange out;
    if (!data.valid()) {
        out.empty = true;
        out.lo = centered ? -1 : 0;
        out.hi = 1;
        return out;
    }
    double lo = data.lo, hi = data.hi;
    if (centered) {
        const double m = std::max(std::abs(lo), std::abs(hi));
        lo = -m;
        hi = m;
    }
    if (!(hi > lo)) {
        out.constant = true;
        out.constantValue = data.lo;
        const double pad = std::abs(data.lo) > 0 ? 1e-6 * std::abs(data.lo) + std::abs(data.lo) * 0.01 : 1.0;
        lo = data.lo - pad;
        hi = data.lo + pad;
    } else if (data.hi == data.lo) {
        out.constant = true;
        out.constantValue = data.lo;
    }
    out.lo = lo;
    out.hi = hi;
    return out;
}

bool logScaleValid(const Range& r) { return r.valid() && r.lo > 0; }

double symlogLinearThreshold(double lo, double hi) {
    const double m = std::max(std::abs(lo), std::abs(hi));
    return m > 0 ? 1e-3 * m : 1.0;
}

double normalizedPosition(double v, double lo, double hi, ScaleMode scale, double c) {
    if (!(v == v)) return kNaN;
    auto clamp = [](double t) { return std::clamp(t, 0.0, 1.0); };
    switch (scale) {
        case ScaleMode::Linear: return hi > lo ? clamp((v - lo) / (hi - lo)) : 0.5;
        case ScaleMode::Log: {
            if (!(lo > 0) || !(hi > lo)) return 0.5;
            const double x = std::log10(std::max(v, lo));
            return clamp((x - std::log10(lo)) / (std::log10(hi) - std::log10(lo)));
        }
        case ScaleMode::SymLog: {
            auto f = [&](double x) { return (x < 0 ? -1.0 : 1.0) * std::log10(1.0 + std::abs(x) / c); };
            const double a = f(lo), b = f(hi);
            return b > a ? clamp((f(v) - a) / (b - a)) : 0.5;
        }
    }
    return 0.5;
}

// ---- labels ----------------------------------------------------------------------------------------------------

namespace {
/// "STEEL: E" -> {"E", "STEEL"}; "E" -> {"E", ""}.
std::pair<std::string, std::string> splitSymbol(const std::string& name) {
    const auto k = name.rfind(": ");
    if (k == std::string::npos || k + 2 >= name.size()) return {name, ""};
    return {name.substr(k + 2), name.substr(0, k)};
}
}  // namespace

std::string parameterSymbol(const ResultStore& store, int p) {
    if (p < 0 || static_cast<size_t>(p) >= store.parameters.size()) return "a" + std::to_string(p);
    const auto& info = store.parameters[static_cast<size_t>(p)];
    const auto [sym, ctx] = splitSymbol(info.name);
    int same = 0;
    for (const auto& q : store.parameters) same += splitSymbol(q.name).first == sym ? 1 : 0;
    if (same > 1) {
        if (!ctx.empty()) return sym + "(" + ctx + ")";
        return sym + "(" + (info.association.empty() ? info.id : info.association) + ")";
    }
    return sym;
}

std::string notationLabel(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d) {
    std::string q = quantityName(store, o);
    if (o.magnitude) q = "|" + store.fields[static_cast<size_t>(o.field)].name + "|";
    if (d.order() == 0) return q + " (nominal)";
    const std::string ks = superscript(d.order());
    if (d.isPure()) return "d" + ks + q + "/d" + parameterSymbol(store, d.params[0]) + ks;
    std::string den;
    for (int p : d.params) den += "∂" + parameterSymbol(store, p);
    return "∂" + ks + q + "/" + den;
}

std::string unitsLine(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d) {
    const auto* fd = fieldOf(store, o.field);
    if (!fd) return "";
    const std::string q = o.magnitude ? "|" + fd->name + "|" : quantityName(store, o);
    if (d.order() == 0) return fd->units.empty() ? "model units of " + q : fd->units;
    const int p = d.params.front();
    const std::string pu =
        (p >= 0 && static_cast<size_t>(p) < store.parameters.size()) ? store.parameters[static_cast<size_t>(p)].units : "";
    const std::string sym = parameterSymbol(store, p);
    const std::string ks = superscript(d.order());
    if (!fd->units.empty() && !pu.empty() && d.isPure()) return fd->units + " per " + pu + ks;
    const std::string num = fd->units.empty() ? "model units of " + q : fd->units;
    if (!d.isPure()) return num + " per product of the parameters' model units";
    if (d.order() == 1) return num + " per model unit of " + sym;
    return num + " per (model unit of " + sym + ")" + ks;
}

namespace {
std::string plainOutput(const ResultStore& store, const OutputChoice& o) {
    const auto* fd = fieldOf(store, o.field);
    if (!fd) return "?";
    const std::string q = quantityName(store, o);
    if (fd->name == "U") {
        if (o.magnitude) return "total displacement";
        static const char* ax[] = {"X", "Y", "Z"};
        const char last = q.empty() ? '0' : q.back();
        if (last >= '1' && last <= '3') return std::string("displacement in ") + ax[last - '1'] + " (" + q + ")";
        return "displacement " + q;
    }
    if (fd->name == "RF") return o.magnitude ? "total reaction force" : "reaction force " + q;
    if (fd->name == "S_Mises" || q == "Mises") return "von Mises stress";
    if (fd->name == "S") return "stress component " + q;
    if (fd->name == "E" || fd->name == "LE") return "strain component " + q;
    if (fd->name == "PEEQ") return "equivalent plastic strain (PEEQ)";
    if (fd->name.rfind("SDV", 0) == 0) return "state variable " + q;
    if (!fd->description.empty()) return fd->description + " " + q;
    return q;
}

std::string plainParam(const ResultStore& store, int p) {
    if (p < 0 || static_cast<size_t>(p) >= store.parameters.size()) return "the parameter";
    const auto& info = store.parameters[static_cast<size_t>(p)];
    if (info.description.empty()) return parameterSymbol(store, p);
    std::string desc = info.description;
    // Plain sentence only: drop keyword/provenance details such as " (*ELASTIC, ...). (model.inp:19, field 1)".
    const auto cut = std::min(desc.find(" ("), desc.find(". "));
    if (cut != std::string::npos && cut > 0) desc = desc.substr(0, cut);
    while (!desc.empty() && (desc.back() == '.' || desc.back() == ' ')) desc.pop_back();
    // Lower-case the first letter unless it starts an acronym or proper name ("UMAT", "STEEL").
    if (desc.size() > 1 && std::isupper(static_cast<unsigned char>(desc[0])) && !std::isupper(static_cast<unsigned char>(desc[1])))
        desc[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(desc[0])));
    return desc + " (" + parameterSymbol(store, p) + ")";
}
}  // namespace

std::string questionSentence(const ResultStore& store, const OutputChoice& o, const DerivativeSpec& d, DataAssoc where) {
    const std::string out = plainOutput(store, o);
    const std::string wh = where == DataAssoc::Point ? "at the nodes" : "in each element";
    switch (d.order()) {
        case 0: return "What is the " + out + " " + wh + "?";
        case 1: return "How much does the " + out + " change when " + plainParam(store, d.params[0]) + " increases?";
        case 2: return "How does the rate of change of the " + out + " itself change as " + plainParam(store, d.params[0]) +
                       " increases? (second order)";
        case 3: return "How does the second-order rate of the " + out + " change as " + plainParam(store, d.params[0]) +
                       " increases? (third order)";
        default: return "Order-" + std::to_string(d.order()) + " derivative of the " + out + ".";
    }
}

std::string formatValue(double v, int significant) {
    if (!(v == v)) return "no data";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", significant, v);
    return buf;
}

std::string frameLabel(const ResultStore& store, int frame) {
    if (store.frames.empty()) return "No increments";
    if (frame < 0 || static_cast<size_t>(frame) >= store.frames.size()) return "?";
    const auto& f = store.frames[static_cast<size_t>(frame)];
    const int n = static_cast<int>(store.frames.size()) - 1;
    std::string s = "Increment " + std::to_string(frame) + " of " + std::to_string(n);
    if (frame == 0) s += " (initial state)";
    s += " · Step \"" + f.stepName + "\" · step time " + formatValue(f.stepTime) + " · total time " + formatValue(f.totalTime);
    return s;
}

}  // namespace abss::viz
