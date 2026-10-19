#include "abaqus_loads.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace abss::abq {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string> kAllowedStepKeywords = {
    "*STEP", "*STATIC", "*END STEP", "*BOUNDARY", "*CLOAD", "*DLOAD", "*DSLOAD", "*OUTPUT", "*NODE OUTPUT",
    "*ELEMENT OUTPUT", "*NODE PRINT", "*EL PRINT", "*NODE FILE", "*EL FILE", "*ENERGY OUTPUT", "*ENERGY PRINT",
    "*ENERGY FILE", "*RESTART", "*MONITOR", "*CONTROLS", "*SOLUTION TECHNIQUE", "*FILE FORMAT", "*CONTACT OUTPUT",
    "*PRINT", "*FILE OUTPUT", "*ELEMENT MATRIX OUTPUT", "*SECTION PRINT", "*SECTION FILE"};

}  // namespace

double LoadEvaluator::Amp::at(double t) const {
    if (pts.empty()) return 0;
    if (t <= pts.front().first) return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (t <= pts[i].first) {
            const auto& [t0, a0] = pts[i - 1];
            const auto& [t1, a1] = pts[i];
            return t1 > t0 ? a0 + (a1 - a0) * (t - t0) / (t1 - t0) : a1;
        }
    return pts.back().second;
}

LoadEvaluator::LoadEvaluator(const inp::Deck& deck) : deck_(deck) {
    const auto& blocks = deck.blocks();
    for (const auto& b : blocks) {
        if (b.keyword != "*AMPLITUDE") continue;
        const auto name = upper(b.param("NAME").value_or(""));
        const auto def = upper(b.param("DEFINITION").value_or("TABULAR"));
        if (def != "TABULAR") throw std::runtime_error("Amplitude " + name + ": DEFINITION=" + def + " is not supported by the Abaqus backend yet");
        if (upper(b.param("VALUE").value_or("RELATIVE")) != "RELATIVE")
            throw std::runtime_error("Amplitude " + name + ": VALUE=ABSOLUTE is not supported by the Abaqus backend yet");
        Amp a;
        a.totalTime = upper(b.param("TIME").value_or("STEP TIME")) == "TOTAL TIME";
        std::vector<double> nums;
        for (const auto& l : b.data)
            for (const auto& f : inp::splitFields(l.text))
                if (auto v = inp::parseNumber(f)) nums.push_back(*v);
        for (size_t i = 0; i + 1 < nums.size(); i += 2) a.pts.emplace_back(nums[i], nums[i + 1]);
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
        if (upper(st.procedure) != "*STATIC")
            throw std::runtime_error("Step '" + st.name + "' uses " + st.procedure + "; the Abaqus backend currently supports *STATIC only");
        steps_[s].period = st.timePeriod;
        const auto& sb = blocks.at(static_cast<size_t>(st.blockIndex));
        steps_[s].ramp = upper(sb.param("AMPLITUDE").value_or("RAMP")) == "RAMP";
        for (int bi = st.blockIndex; bi <= st.endBlockIndex && bi >= 0; ++bi) {
            const auto& b = blocks.at(static_cast<size_t>(bi));
            if (!kAllowedStepKeywords.count(b.keyword))
                throw std::runtime_error("Step '" + st.name + "' uses " + b.keyword + ", which the Abaqus backend cannot replay yet");
            if (b.keyword == "*STATIC" && b.hasParam("RIKS")) throw std::runtime_error("*STATIC, RIKS is not supported");
        }
    }
    // Loads (step data only).
    for (const auto& b : blocks) {
        if (b.keyword != "*CLOAD" && b.keyword != "*DLOAD" && b.keyword != "*DSLOAD") continue;
        if (b.stepIndex < 0) continue;
        auto& sd = steps_.at(static_cast<size_t>(b.stepIndex));
        if (b.hasParam("FOLLOWER"))
            throw std::runtime_error(b.keyword + ", FOLLOWER (line " + std::to_string(b.line) +
                                     ") is not replayed by the Abaqus backend yet (it would become a fixed-direction load)");
        if (upper(b.param("OP").value_or("MOD")) == "NEW") sd.opNew.push_back(b.keyword);
        for (const auto& l : b.data) {
            Entry e;
            e.keyword = b.keyword;
            e.fields = inp::splitFields(l.text);
            e.amplitude = upper(b.param("AMPLITUDE").value_or(""));
            e.stepDefined = b.stepIndex;
            e.magField = 2;
            if (e.fields.size() < 3) continue;  // e.g. "*DLOAD" with omitted magnitude -> zero; skip safely
            e.magnitude = inp::parseNumber(e.fields[2]).value_or(0.0);
            e.key = e.keyword + "|" + upper(e.fields[0]) + "|" + upper(e.fields[1]);
            if (b.keyword == "*CLOAD") {
                e.nodes = deck.resolveNodes(e.fields[0], "");
                e.dof = static_cast<int>(inp::parseNumber(e.fields[1]).value_or(0));
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
    prescribedDofs();  // validates BC-set consistency
}

double LoadEvaluator::value(const Entry& e, int step, double tau, double total, double prev, bool definedNow) const {
    const auto& sd = steps_.at(static_cast<size_t>(step));
    if (!e.amplitude.empty()) {
        const auto& a = amps_.at(e.amplitude);
        if (a.totalTime) return e.magnitude * a.at(total);
        if (definedNow) return e.magnitude * a.at(tau);
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
                now[k] = sd.ramp ? a.end * (1.0 - (sd.period > 0 ? tau / sd.period : 1.0)) : 0.0;
                continue;  // gone after this step
            }
            const double v = value(a.e, s, tau, tot, a.end, false);
            now[k] = v;
            next[k] = {a.e, v};
        }
        for (const auto& e : sd.entries) {
            double prev = 0;
            // A respecified quantity ramps from its end-of-previous-step value, also under OP=NEW (runtime self-check
            // against the nominal job's own CF/U output guards this assumption).
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
            l.fields[static_cast<size_t>(e->magField)] = inp::formatNumber(v);
            st.loads.push_back(std::move(l));
            if (e->keyword == "*CLOAD")
                for (auto n : e->nodes) st.concentrated[{n, e->dof}] += v;
        }
    }
    return st;
}

std::vector<std::pair<std::int32_t, int>> LoadEvaluator::prescribedDofs() const {
    std::set<std::pair<std::int32_t, int>> all, current, first;
    bool haveFirst = false;
    for (const auto& e : modelBcs_) current.insert({e.nodes.at(0), e.dof});
    all = current;
    for (size_t s = 0; s < steps_.size(); ++s) {
        const auto& sd = steps_[s];
        std::set<std::pair<std::int32_t, int>> defined;
        for (const auto& e : sd.entries)
            if (e.keyword == "*BOUNDARY") defined.insert({e.nodes.at(0), e.dof});
        if (std::find(sd.opNew.begin(), sd.opNew.end(), "*BOUNDARY") != sd.opNew.end()) {
            std::set<std::pair<std::int32_t, int>> kept;
            for (const auto& e : modelBcs_) kept.insert({e.nodes.at(0), e.dof});
            kept.insert(defined.begin(), defined.end());
            for (const auto& d : current)
                if (!kept.count(d))
                    throw std::runtime_error("A boundary condition is released (OP=NEW) in step " + std::to_string(s + 1) +
                                             "; changing constrained DOF sets are not supported by the Abaqus backend yet");
            current = kept;
        } else {
            current.insert(defined.begin(), defined.end());
        }
        all.insert(current.begin(), current.end());
        if (!haveFirst) {
            first = current;
            haveFirst = true;
        }
    }
    if (haveFirst && first != current)
        throw std::runtime_error("The set of constrained DOFs changes between steps; not supported by the Abaqus backend yet");
    return {all.begin(), all.end()};
}

}  // namespace abss::abq
