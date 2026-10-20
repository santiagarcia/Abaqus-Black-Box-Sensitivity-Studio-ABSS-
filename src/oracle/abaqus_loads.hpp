// Evaluation of an INP's step-wise boundary conditions and loads at a given (step, step time) so that each oracle
// increment can re-impose them explicitly (OP=NEW) in a single-increment restart step. Supported subset:
// *BOUNDARY (displacement), *CLOAD, *DLOAD, *DSLOAD with OP=MOD/NEW, *AMPLITUDE (TABULAR, STEP/TOTAL TIME), step
// AMPLITUDE=RAMP/STEP in *STATIC steps. Everything is cross-checked against the nominal job's own output.
#pragma once

#include "abss/inp/deck.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace abss::abq {

struct LoadLine {
    std::string keyword;              ///< "*CLOAD", "*DLOAD", "*DSLOAD"
    std::vector<std::string> fields;  ///< data line fields with the magnitude replaced
    bool follower = false;            ///< FOLLOWER (re-stated as such in every oracle step)
};

struct LoadState {
    std::map<std::pair<std::int32_t, int>, double> bc;  ///< (mesh node, dof) -> prescribed value
    std::vector<LoadLine> loads;
    std::map<std::pair<std::int32_t, int>, double> concentrated;  ///< resolved fixed-direction CLOAD sums (for checks)
    std::set<std::pair<std::int32_t, int>> followerDofs;           ///< DOFs carrying follower CLOADs (not checked vs CF)
};

class LoadEvaluator {
public:
    /// Throws std::runtime_error naming the first unsupported feature.
    explicit LoadEvaluator(const inp::Deck& deck);
    /// State at the end of time `stepTime` into step `stepIndex` (0-based) with total time `totalTime`.
    LoadState evaluate(int stepIndex, double stepTime, double totalTime) const;
    /// DOFs prescribed in any step (union of the per-step constrained sets).
    std::vector<std::pair<std::int32_t, int>> prescribedDofs() const;
    /// Constrained DOFs during each step (0-based). A DOF missing from step s but present in s-1 is released at the
    /// start of s; Abaqus then ramps its reaction force down over step s (RAMP) or removes it at once (STEP).
    std::vector<std::set<std::pair<std::int32_t, int>>> constrainedByStep() const;
    bool stepRamps(int step) const { return steps_.at(static_cast<size_t>(step)).ramp; }
    double stepPeriod(int step) const { return steps_.at(static_cast<size_t>(step)).period; }

private:
    struct Amp {
        enum class Kind { Tabular, EquallySpaced, Periodic, Modulated, Decay, SmoothStep } kind = Kind::Tabular;
        bool totalTime = false;
        bool absolute = false;  ///< VALUE=ABSOLUTE: the amplitude is the value itself (data-line magnitude ignored)
        double smooth = 0;      ///< TABULAR SMOOTH fraction (quadratic blending around interior points)
        std::vector<std::pair<double, double>> pts;  ///< Tabular / SmoothStep / EquallySpaced (expanded) points
        std::vector<double> par;                     ///< Periodic / Modulated / Decay parameters (data-line order)
        double at(double t) const;
    };
    struct Entry {
        std::string keyword;
        std::string key;
        std::vector<std::string> fields;
        int magField = -1;
        double magnitude = 0;
        std::string amplitude;
        int stepDefined = 0;
        std::vector<std::int32_t> nodes;  ///< for BC / CLOAD
        int dof = 0;
        bool follower = false;
    };
    double value(const Entry& e, int step, double tau, double total, double prev, bool definedNow) const;

    const inp::Deck& deck_;
    std::map<std::string, Amp> amps_;
    struct StepDef {
        double period = 1;
        bool ramp = true;
        std::vector<Entry> entries;     ///< entries (re)defined in this step
        std::vector<std::string> opNew; ///< keywords with OP=NEW in this step
    };
    std::vector<StepDef> steps_;
    std::vector<Entry> modelBcs_;
    std::map<std::string, double> initialTemp_;  ///< *TEMPERATURE entry key -> initial temperature of its nodes
};

}  // namespace abss::abq
