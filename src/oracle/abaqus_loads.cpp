#include "abaqus_loads.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>

namespace abss::abq {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string> kAllowedStepKeywords = {
    "*STEP", "*STATIC", "*VISCO", "*DYNAMIC", "*END STEP", "*BOUNDARY", "*CLOAD", "*DLOAD", "*DSLOAD", "*TEMPERATURE", "*OUTPUT", "*NODE OUTPUT",
    "*ELEMENT OUTPUT", "*NODE PRINT", "*EL PRINT", "*NODE FILE", "*EL FILE", "*ENERGY OUTPUT", "*ENERGY PRINT",
    "*ENERGY FILE", "*RESTART", "*MONITOR", "*CONTROLS", "*SOLUTION TECHNIQUE", "*FILE FORMAT", "*CONTACT OUTPUT",
    "*PRINT", "*FILE OUTPUT", "*ELEMENT MATRIX OUTPUT", "*SECTION PRINT", "*SECTION FILE", "*CONTACT CONTROLS",
    "*CONTACT INTERFERENCE", "*CHANGE FRICTION", "*CONTACT PRINT", "*CONTACT FILE", "*CONTACT RESPONSE",
    "*INTEGRATED OUTPUT", "*INCREMENTATION OUTPUT", "*DIAGNOSTICS", "*EL FILE", "*NODE RESPONSE", "*ELEMENT RESPONSE"};

}  // namespace

double LoadEvaluator::Amp::at(double t) const {
    switch (kind) {
        case Kind::Periodic: {  // N, omega, t0, A0, then A_i, B_i pairs
            if (par.size() < 4) return 0;
            const int n = static_cast<int>(par[0]);
            const double w = par[1], t0 = par[2], a0 = par[3];
            if (t < t0) return a0;
            double a = a0;
            for (int i = 1; i <= n && 4 + 2 * i - 1 < static_cast<int>(par.size()); ++i)
                a += par[static_cast<size_t>(2 + 2 * i)] * std::cos(i * w * (t - t0)) + par[static_cast<size_t>(3 + 2 * i)] * std::sin(i * w * (t - t0));
            return a;
        }
        case Kind::Modulated: {  // A0, A, t0, omega1, omega2
            if (par.size() < 5) return 0;
            if (t <= par[2]) return par[0];
            return par[0] + par[1] * std::sin(par[3] * (t - par[2])) * std::sin(par[4] * (t - par[2]));
        }
        case Kind::Decay: {  // A0, A, t0, td
            if (par.size() < 4) return 0;
            if (t < par[2]) return par[0];
            return par[0] + par[1] * std::exp(-(t - par[2]) / par[3]);
        }
        default: break;
    }
    if (pts.empty()) return 0;
    if (t <= pts.front().first) return pts.front().second;
    if (t >= pts.back().first) return pts.back().second;
    size_t i = 1;
    while (i < pts.size() && t > pts[i].first) ++i;
    const auto [t0, a0] = pts[i - 1];
    const auto [t1, a1] = pts[i];
    if (!(t1 > t0)) return a1;
    if (kind == Kind::SmoothStep) {  // a0 + (a1 - a0) xi^3 (10 - 15 xi + 6 xi^2)
        const double x = (t - t0) / (t1 - t0);
        return a0 + (a1 - a0) * x * x * x * (10 - 15 * x + 6 * x * x);
    }
    auto lin = [&](size_t k, double tt) {
        const auto [u0, b0] = pts[k - 1];
        const auto [u1, b1] = pts[k];
        return u1 > u0 ? b0 + (b1 - b0) * (tt - u0) / (u1 - u0) : b1;
    };
    if (smooth > 0) {
        // Quadratic blend of the two linear pieces within +- smooth * (adjacent interval) of each interior point.
        for (size_t k : {i - 1, i}) {
            if (k == 0 || k + 1 >= pts.size()) continue;
            const double tk = pts[k].first, l = tk - smooth * (tk - pts[k - 1].first), r = tk + smooth * (pts[k + 1].first - tk);
            if (t > l && t < r) {
                const double p0 = lin(k, l), p1 = pts[k].second, p2 = lin(k + 1, r);
                // t(u) = (1-u)^2 l + 2u(1-u) tk + u^2 r  ->  solve for u in [0, 1]
                const double A = l - 2 * tk + r, B = 2 * (tk - l), C = l - t;
                const double u = std::abs(A) < 1e-300 ? -C / B : (-B + std::sqrt(std::max(0.0, B * B - 4 * A * C))) / (2 * A);
                return (1 - u) * (1 - u) * p0 + 2 * u * (1 - u) * p1 + u * u * p2;
            }
        }
    }
    return lin(i, t);
}

LoadEvaluator::LoadEvaluator(const inp::Deck& deck) : deck_(deck) {
    const auto& blocks = deck.blocks();
    for (const auto& b : blocks) {
        if (b.keyword != "*AMPLITUDE") continue;
        const auto name = upper(b.param("NAME").value_or(""));
        const auto def = upper(b.param("DEFINITION").value_or("TABULAR"));
        Amp a;
        a.totalTime = upper(b.param("TIME").value_or("STEP TIME")) == "TOTAL TIME";
        a.absolute = upper(b.param("VALUE").value_or("RELATIVE")) == "ABSOLUTE";
        if (auto s = b.param("SMOOTH")) a.smooth = inp::parseNumber(*s).value_or(0.0);
        std::vector<double> nums;
        for (const auto& l : b.data)
            for (const auto& f : inp::splitFields(l.text))
                if (auto v = inp::parseNumber(f)) nums.push_back(*v);
        if (def == "TABULAR" || def == "SMOOTH STEP") {
            a.kind = def == "TABULAR" ? Amp::Kind::Tabular : Amp::Kind::SmoothStep;
            for (size_t i = 0; i + 1 < nums.size(); i += 2) a.pts.emplace_back(nums[i], nums[i + 1]);
        } else if (def == "EQUALLY SPACED") {
            a.kind = Amp::Kind::EquallySpaced;
            const double dt = inp::parseNumber(b.param("FIXED INTERVAL").value_or("0")).value_or(0.0);
            const double t0 = inp::parseNumber(b.param("BEGIN").value_or("0")).value_or(0.0);
            if (!(dt > 0)) throw std::runtime_error("Amplitude " + name + ": EQUALLY SPACED needs FIXED INTERVAL > 0");
            for (size_t i = 0; i < nums.size(); ++i) a.pts.emplace_back(t0 + dt * static_cast<double>(i), nums[i]);
        } else if (def == "PERIODIC" || def == "MODULATED" || def == "DECAY") {
            a.kind = def == "PERIODIC" ? Amp::Kind::Periodic : def == "MODULATED" ? Amp::Kind::Modulated : Amp::Kind::Decay;
            a.par = nums;
        } else {
            throw std::runtime_error("Amplitude " + name + ": DEFINITION=" + def + " (" +
                                     (def == "SOLUTION DEPENDENT" || def == "BUBBLE" || def == "USER" || def == "ACTUATOR"
                                          ? "its value depends on the solution or on user code at run time"
                                          : "not an analytic time function") +
                                     ") cannot be replayed exactly by the Abaqus backend.");
        }
        amps_[name] = a;
    }
    for (const auto& b : blocks) {
        if (!b.partName.empty() && (b.keyword == "*CLOAD" || b.keyword == "*DLOAD" || b.keyword == "*BOUNDARY"))
            throw std::runtime_error("Loads/BCs inside *PART are not supported by the Abaqus backend yet");
    }
    steps_.resize(deck.steps().size());
    for (size_t s = 0; s < deck.steps().size(); ++s) {
        const auto& st = deck.steps()[s];
        if (st.perturbation) throw std::runtime_error("Linear perturbation steps are not supported by the Abaqus backend");
        const std::string proc = upper(st.procedure);
        if (proc != "*STATIC" && proc != "*VISCO" && proc != "*DYNAMIC")
            throw std::runtime_error("Step '" + st.name + "' uses " + st.procedure + "; the Abaqus backend supports *STATIC, "
                                     "*VISCO and implicit *DYNAMIC steps (" + st.procedure + " has no displacement-residual "
                                     "form the oracle can evaluate)");
        steps_[s].period = st.timePeriod;
        const auto& sb = blocks.at(static_cast<size_t>(st.blockIndex));
        // Abaqus/Standard default load variation: RAMP, except STEP for transient dynamics (*DYNAMIC) and for *VISCO
        // (measured: a *VISCO CLOAD of 60 is fully applied at t = 0.5 of a 2.0 step); an explicit AMPLITUDE= on *STEP
        // wins. The run-time self-check against the nominal CF/U output guards this rule.
        bool dynamicStep = proc == "*DYNAMIC" || proc == "*VISCO";
        for (int bi = st.blockIndex; bi <= st.endBlockIndex; ++bi) {
            const auto& b = blocks.at(static_cast<size_t>(bi));
            if (b.keyword == "*DYNAMIC") {
                if (b.hasParam("EXPLICIT")) throw std::runtime_error("*DYNAMIC, EXPLICIT is not supported (no implicit residual)");
                const auto app = upper(b.param("APPLICATION").value_or("TRANSIENT FIDELITY"));
                if (app == "QUASI-STATIC") dynamicStep = false;  // Abaqus uses RAMP for quasi-static applications
            }
        }
        steps_[s].ramp = upper(sb.param("AMPLITUDE").value_or(dynamicStep ? "STEP" : "RAMP")) == "RAMP";
        for (int bi = st.blockIndex; bi <= st.endBlockIndex && bi >= 0; ++bi) {
            const auto& b = blocks.at(static_cast<size_t>(bi));
            if (!kAllowedStepKeywords.count(b.keyword))
                throw std::runtime_error("Step '" + st.name + "' uses " + b.keyword + ", which the Abaqus backend cannot replay yet");
            if (b.keyword == "*STATIC" && b.hasParam("RIKS")) throw std::runtime_error("*STATIC, RIKS is not supported");
        }
    }
    // Initial temperatures (*INITIAL CONDITIONS, TYPE=TEMPERATURE: node or set, value).
    std::map<std::int32_t, double> initialTemp;
    for (const auto& b : blocks) {
        if (b.keyword != "*INITIAL CONDITIONS" || upper(b.param("TYPE").value_or("")) != "TEMPERATURE") continue;
        if (b.hasParam("FILE") || b.hasParam("USER"))
            throw std::runtime_error("*INITIAL CONDITIONS, TYPE=TEMPERATURE with FILE= or USER cannot be re-evaluated by ABSS");
        for (const auto& l : b.data) {
            const auto f = inp::splitFields(l.text);
            if (f.size() < 2) continue;
            const double v = inp::parseNumber(f[1]).value_or(0.0);
            for (auto n : deck.resolveNodes(f[0], b.instanceName)) initialTemp[n] = v;
        }
    }
    // Loads and predefined temperatures (step data only).
    for (const auto& b : blocks) {
        if (b.keyword != "*CLOAD" && b.keyword != "*DLOAD" && b.keyword != "*DSLOAD" && b.keyword != "*TEMPERATURE") continue;
        if (b.stepIndex < 0) continue;
        if (b.keyword == "*TEMPERATURE" && (b.hasParam("FILE") || b.hasParam("USER")))
            throw std::runtime_error("*TEMPERATURE with FILE= or USER (line " + std::to_string(b.line) +
                                     ") reads temperatures from another analysis or user code; ABSS cannot re-evaluate them "
                                     "at its own increments. Give the temperatures as values (with an amplitude if needed).");
        auto& sd = steps_.at(static_cast<size_t>(b.stepIndex));
        if (upper(b.param("OP").value_or("MOD")) == "NEW") sd.opNew.push_back(b.keyword);
        for (const auto& l : b.data) {
            Entry e;
            e.keyword = b.keyword;
            // FOLLOWER: the direction follows the node's total rotation, so restating it in a restart step reproduces it.
            e.follower = b.hasParam("FOLLOWER");
            e.fields = inp::splitFields(l.text);
            e.amplitude = upper(b.param("AMPLITUDE").value_or(""));
            e.stepDefined = b.stepIndex;
            e.magField = b.keyword == "*TEMPERATURE" ? 1 : 2;  // *TEMPERATURE: node/set, value
            if (e.fields.size() < static_cast<size_t>(e.magField) + 1) continue;  // omitted magnitude -> zero; skip safely
            e.magnitude = inp::parseNumber(e.fields[static_cast<size_t>(e.magField)]).value_or(0.0);
            e.key = e.keyword + "|" + upper(e.fields[0]) + "|" + (e.magField == 2 ? upper(e.fields[1]) : std::string());
            if (b.keyword == "*CLOAD") {
                e.nodes = deck.resolveNodes(e.fields[0], "");
                e.dof = static_cast<int>(inp::parseNumber(e.fields[1]).value_or(0));
            }
            if (b.keyword == "*TEMPERATURE") {
                // Ramps start from the initial temperature of these nodes, which must be one value for the set.
                std::optional<double> t0;
                for (auto n : deck.resolveNodes(e.fields[0], b.instanceName)) {
                    const auto it = initialTemp.find(n);
                    const double v = it == initialTemp.end() ? 0.0 : it->second;
                    if (t0 && std::abs(*t0 - v) > 1e-12 * std::max(1.0, std::abs(v)))
                        throw std::runtime_error("*TEMPERATURE " + e.fields[0] + " (line " + std::to_string(b.line) +
                                                 "): its nodes start from different initial temperatures; define the "
                                                 "temperature per node or per uniformly initialised set.");
                    t0 = v;
                }
                initialTemp_[e.key] = t0.value_or(0.0);
            }
            if (!e.amplitude.empty() && !amps_.count(e.amplitude))
                throw std::runtime_error("Unknown amplitude " + e.amplitude);
            sd.entries.push_back(std::move(e));
        }
    }
    // Boundary conditions.
    for (const auto& be : deck.boundaries()) {
        if (!be.type.empty() && upper(be.type) != "DISPLACEMENT")
            throw std::runtime_error("*BOUNDARY, TYPE=" + be.type + " is not supported by the Abaqus backend");
        for (auto n : be.nodes)
            for (int d = be.firstDof; d <= be.lastDof; ++d) {
                Entry e;
                e.keyword = "*BOUNDARY";
                e.key = "BC|" + std::to_string(n) + "|" + std::to_string(d);
                e.magnitude = be.magnitude;
                e.amplitude = upper(be.amplitude);
                e.stepDefined = be.stepIndex;
                e.nodes = {n};
                e.dof = d;
                if (!e.amplitude.empty() && !amps_.count(e.amplitude)) throw std::runtime_error("Unknown amplitude " + e.amplitude);
                if (be.stepIndex < 0) {
                    modelBcs_.push_back(e);
                } else {
                    auto& sd = steps_.at(static_cast<size_t>(be.stepIndex));
                    if (upper(be.op) == "NEW" &&
                        std::find(sd.opNew.begin(), sd.opNew.end(), "*BOUNDARY") == sd.opNew.end())
                        sd.opNew.push_back("*BOUNDARY");
                    sd.entries.push_back(e);
                }
            }
    }
}

double LoadEvaluator::value(const Entry& e, int step, double tau, double total, double prev, bool definedNow) const {
    const auto& sd = steps_.at(static_cast<size_t>(step));
    if (!e.amplitude.empty()) {
        const auto& a = amps_.at(e.amplitude);
        const double scale = a.absolute ? 1.0 : e.magnitude;
        if (a.totalTime) return scale * a.at(total);
        if (definedNow) return scale * a.at(tau);
        return prev;  // propagated: stays at its end-of-previous-step value (cross-checked against nominal output)
    }
    if (!definedNow) return prev;
    return sd.ramp ? prev + (e.magnitude - prev) * (sd.period > 0 ? tau / sd.period : 1.0) : e.magnitude;
}

LoadState LoadEvaluator::evaluate(int stepIndex, double stepTime, double totalTime) const {
    struct Active {
        Entry e;
        double end = 0;  // value at end of last processed step
    };
    std::map<std::string, Active> act;
    for (const auto& e : modelBcs_) act[e.key] = {e, e.magnitude};
    double startTotal = 0;
    std::map<std::string, double> now;  // values at the evaluation time
    for (int s = 0; s <= stepIndex; ++s) {
        const auto& sd = steps_.at(static_cast<size_t>(s));
        const bool last = s == stepIndex;
        const double tau = last ? stepTime : sd.period;
        const double tot = last ? totalTime : startTotal + sd.period;
        std::map<std::string, Active> next;
        now.clear();
        std::set<std::string> redefined;
        for (const auto& e : sd.entries) redefined.insert(e.key);
        for (auto& [k, a] : act) {
            if (redefined.count(k)) continue;
            const bool removed = std::find(sd.opNew.begin(), sd.opNew.end(), a.e.keyword) != sd.opNew.end() &&
                                 a.e.stepDefined >= 0;
            if (removed) {
                // A removed load ramps down over the step (a temperature returns to its initial value). A removed BC
                // frees its DOF (its reaction force is ramped down by the oracle, which knows the branch's reaction).
                const double base = a.e.keyword == "*TEMPERATURE" ? initialTemp_.at(k) : 0.0;
                if (a.e.keyword != "*BOUNDARY")
                    now[k] = sd.ramp ? base + (a.end - base) * (1.0 - (sd.period > 0 ? tau / sd.period : 1.0)) : base;
                continue;  // gone after this step
            }
            const double v = value(a.e, s, tau, tot, a.end, false);
            now[k] = v;
            next[k] = {a.e, v};
        }
        for (const auto& e : sd.entries) {
            // A respecified quantity ramps from its end-of-previous-step value, also under OP=NEW (runtime self-check
            // against the nominal job's own CF/U output guards this assumption); a new temperature from the initial one.
            double prev = e.keyword == "*TEMPERATURE" ? initialTemp_.at(e.key) : 0.0;
            if (auto it = act.find(e.key); it != act.end()) prev = it->second.end;
            const double v = value(e, s, tau, tot, prev, true);
            now[e.key] = v;
            next[e.key] = {e, v};
        }
        // entries carried keep value 'now' as end value for the next step when this is not the last step
        act = std::move(next);
        startTotal += sd.period;
    }
    if (stepIndex < 0)
        for (auto& [k, a] : act) now[k] = a.end;

    LoadState st;
    std::map<std::string, const Entry*> byKey;
    for (const auto& e : modelBcs_) byKey[e.key] = &e;
    for (const auto& sd : steps_)
        for (const auto& e : sd.entries) byKey[e.key] = &e;
    for (const auto& [k, v] : now) {
        const Entry* e = byKey.at(k);
        if (e->keyword == "*BOUNDARY") {
            st.bc[{e->nodes.at(0), e->dof}] = v;
        } else {
            LoadLine l;
            l.keyword = e->keyword;
            l.fields = e->fields;
            l.follower = e->follower;
            l.fields[static_cast<size_t>(e->magField)] = inp::formatNumber(v);
            st.loads.push_back(std::move(l));
            if (e->keyword == "*CLOAD")
                for (auto n : e->nodes) {
                    if (e->follower) st.followerDofs.insert({n, e->dof});  // rotates with the node: not comparable to CF
                    else st.concentrated[{n, e->dof}] += v;
                }
        }
    }
    return st;
}

std::vector<std::set<std::pair<std::int32_t, int>>> LoadEvaluator::constrainedByStep() const {
    std::vector<std::set<std::pair<std::int32_t, int>>> out;
    std::set<std::pair<std::int32_t, int>> current;
    for (const auto& e : modelBcs_) current.insert({e.nodes.at(0), e.dof});
    for (const auto& sd : steps_) {
        std::set<std::pair<std::int32_t, int>> defined;
        for (const auto& e : sd.entries)
            if (e.keyword == "*BOUNDARY") defined.insert({e.nodes.at(0), e.dof});
        if (std::find(sd.opNew.begin(), sd.opNew.end(), "*BOUNDARY") != sd.opNew.end()) {
            // OP=NEW keeps model-data BCs and the BCs restated in this step; everything else is released.
            std::set<std::pair<std::int32_t, int>> kept;
            for (const auto& e : modelBcs_) kept.insert({e.nodes.at(0), e.dof});
            kept.insert(defined.begin(), defined.end());
            current = kept;
        } else {
            current.insert(defined.begin(), defined.end());
        }
        out.push_back(current);
    }
    return out;
}

std::vector<std::pair<std::int32_t, int>> LoadEvaluator::prescribedDofs() const {
    std::set<std::pair<std::int32_t, int>> all;
    for (const auto& e : modelBcs_) all.insert({e.nodes.at(0), e.dof});
    for (const auto& s : constrainedByStep()) all.insert(s.begin(), s.end());
    return {all.begin(), all.end()};
}

}  // namespace abss::abq
