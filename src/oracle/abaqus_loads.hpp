// Evaluation of an INP's step-wise boundary conditions and loads at a given (step, step time) so that each oracle
// increment can re-impose them explicitly (OP=NEW) in a single-increment restart step. Supported subset:
// *BOUNDARY (displacement), *CLOAD, *DLOAD, *DSLOAD with OP=MOD/NEW, *AMPLITUDE (TABULAR, STEP/TOTAL TIME), step
// AMPLITUDE=RAMP/STEP in *STATIC steps. Everything is cross-checked against the nominal job's own output.
#pragma once

#include "abss/inp/deck.hpp"

#include <map>
#include <string>
#include <vector>

namespace abss::abq {

struct LoadLine {
    std::string keyword;              ///< "*CLOAD", "*DLOAD", "*DSLOAD"
    std::vector<std::string> fields;  ///< data line fields with the magnitude replaced
};

struct LoadState {
    std::map<std::pair<std::int32_t, int>, double> bc;  ///< (mesh node, dof) -> prescribed value
    std::vector<LoadLine> loads;
    std::map<std::pair<std::int32_t, int>, double> concentrated;  ///< resolved CLOAD sums (for checks)
};

class LoadEvaluator {
public:
    /// Throws std::runtime_error naming the first unsupported feature.
    explicit LoadEvaluator(const inp::Deck& deck);
    /// State at the end of time `stepTime` into step `stepIndex` (0-based) with total time `totalTime`.
    LoadState evaluate(int stepIndex, double stepTime, double totalTime) const;
    /// DOFs prescribed in any step (the constrained set C); throws if a DOF is released by OP=NEW mid-analysis.
    std::vector<std::pair<std::int32_t, int>> prescribedDofs() const;

private:
    struct Amp {
        bool totalTime = false;
        std::vector<std::pair<double, double>> pts;
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
};

}  // namespace abss::abq
