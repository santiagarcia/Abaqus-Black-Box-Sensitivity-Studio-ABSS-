// Real Abaqus/Standard residual/tangent oracle (mechanism verified on Abaqus 2024: docs/council/scout_abaqus2024.md,
// docs/council/curie_review.md).
//
//  nominal job      : the user's deck (copied, never modified) + *FILE FORMAT, ASCII (double precision U/RF/CF/S/E/PE)
//                     + *ELEMENT MATRIX OUTPUT (per-element stiffness at every converged increment).
//  history branch   : chain of single-increment jobs for parameters a + delta, started from t = 0 (Abaqus silently
//                     ignores material edits on restart). Increment i restarts from the branch's commit job of i-1
//                     (*RESTART, READ, STEP=i-1, INC=1, END STEP), prescribes EVERY independent DOF (*BOUNDARY,
//                     OP=NEW; free DOFs at the requested u, constrained DOFs at their perturbed g_i(a+delta)) and
//                     re-imposes all loads at their t_i values (*CLOAD/*DLOAD/*DSLOAD, OP=NEW). r = -RF.
//  probe            : such a job whose results are read and then discarded (history untouched).
//  commit           : such a job that writes restart data and becomes the branch history for increment i+1.
#include "abss/oracle/abaqus.hpp"

#include "abaqus_files.hpp"
#include "abaqus_loads.hpp"
#include "abaqus_process.hpp"
#include "abss/inp/deck.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <semaphore>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace abss {

std::vector<std::pair<std::string, std::string>> abaqusCapabilities() {
    return {
        {"Residual oracle (all DOFs prescribed, r = -RF)", "Verified on Abaqus 2024 (T2D2, CPS4); see VALIDATION_RESULTS"},
        {"Restart replay of an increment", "Bit-exact on Abaqus 2024 (truss, NLGEOM + plasticity)"},
        {"Tangent from *ELEMENT MATRIX OUTPUT", "Exact for elastic/NLGEOM-elastic; elastic modulus under plasticity -> engine uses matrix-free GMRES"},
        {"Double-precision results", "ASCII .fil, 16 significant digits"},
        {"Procedures", "*STATIC only (no RIKS, dynamics, perturbation steps)"},
        {"Loads/BCs", "*BOUNDARY (displacement), *CLOAD, *DLOAD, *DSLOAD; TABULAR amplitudes; self-checked vs nominal"},
        {"Model structure", "Flat decks (no *PART/*INSTANCE) in this version"},
        {"Constraints/contact", "MPC, *EQUATION, *TIE, *COUPLING, contact: not supported (refused)"},
        {"UMAT/UEL", "Supported in principle with user= (opaque); untested here (no Intel Fortran)"},
    };
}

namespace {


std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write " + p.string());
    out << s;
}

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : t) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

double mises(const std::vector<std::string>& names, const std::vector<double>& s) {
    double c[6] = {0, 0, 0, 0, 0, 0};  // 11 22 33 12 13 23
    static const char* order[] = {"11", "22", "33", "12", "13", "23"};
    for (size_t k = 0; k < names.size() && k < s.size(); ++k)
        for (int q = 0; q < 6; ++q)
            if (names[k].size() >= 2 && names[k].substr(names[k].size() - 2) == order[q]) c[q] = s[k];
    return std::sqrt(0.5 * ((c[0] - c[1]) * (c[0] - c[1]) + (c[1] - c[2]) * (c[1] - c[2]) + (c[2] - c[0]) * (c[2] - c[0])) +
                     3.0 * (c[3] * c[3] + c[4] * c[4] + c[5] * c[5]));
}

std::vector<std::string> tensorNames(const std::string& prefix, size_t n) {
    static const char* n1[] = {"11"};
    static const char* n3[] = {"11", "22", "12"};
    static const char* n4[] = {"11", "22", "33", "12"};
    static const char* n6[] = {"11", "22", "33", "12", "13", "23"};
    const char** src = n == 1 ? n1 : n == 3 ? n3 : n == 4 ? n4 : n6;
    std::vector<std::string> out;
    for (size_t k = 0; k < n; ++k) out.push_back(prefix + (n == 1 || n == 3 || n == 4 || n == 6 ? std::string(src[k]) : std::to_string(k + 1)));
    return out;
}

class AbaqusOracle;

// ---------------------------------------------------------------------------------------------------------------
class AbaqusBranch final : public HistoryBranch {
public:
    AbaqusBranch(AbaqusOracle& o, std::vector<double> delta, int id);
    ~AbaqusBranch() override;
    std::vector<std::vector<double>> residuals(int frame, const std::vector<std::vector<double>>& u) override;
    FrameFields commit(int frame, const std::vector<double>& u, std::vector<double>* committedU) override;
    std::vector<double> lastCommitResidual() const override { return lastResidual_; }

private:
    std::string stepText(int frame, const std::vector<double>& u, bool commit) const;
    abq::FilIncrement runIncrement(int frame, const std::vector<double>& u, bool commit, const std::string& job);

    AbaqusOracle& o_;
    std::vector<double> delta_;
    int id_;
    fs::path dir_;
    std::unique_ptr<inp::Deck> deck_;
    std::unique_ptr<abq::LoadEvaluator> loads_;
    std::string modelData_;
    int committed_ = 0;
    std::string lastCommitJob_;
    std::vector<double> lastResidual_;
    std::atomic<int> probeCounter_{0};
};

class AbaqusOracle final : public Oracle {
public:
    explicit AbaqusOracle(AbaqusRunConfig cfg);
    OracleCapabilities capabilities() const override;
    std::vector<std::vector<double>> runNominal(ResultStore& store, const ProgressFn& progress) override;
    const DofMap& dofMap() const override { return dofs_; }
    CsrMatrix tangent(int frame) override;
    std::unique_ptr<HistoryBranch> openBranch(const std::vector<double>& delta) override;
    std::vector<std::vector<double>> solvePerturbed(const std::vector<double>& delta, std::vector<FrameFields>* fields) override;

    // ---- shared helpers for branches ----
    struct Edited {
        fs::path root;
        std::unique_ptr<inp::Deck> deck;
    };
    Edited editedDeck(const std::vector<double>& delta, const fs::path& dir, const std::string& name) const;
    std::string modelDataOf(const inp::Deck& d, const fs::path& editedRoot) const;
    std::string fieldRequests() const;
    abq::FilIncrement runJob(const fs::path& dir, const std::string& job, const std::string& oldjob, bool expectSingle,
                             double expectTotal, std::vector<abq::FilIncrement>* all = nullptr);
    std::vector<double> dofVector(const std::map<std::int64_t, std::vector<double>>& m) const;
    FrameFields fieldsFrom(const abq::FilIncrement& inc) const;
    void log(const std::string& s) const {
        std::lock_guard<std::mutex> lk(logMutex_);
        if (cfg_.log) cfg_.log(s);
    }
    void cleanup(const fs::path& dir, const std::string& job) const;

    AbaqusRunConfig cfg_;
    std::unique_ptr<inp::Deck> deck_;
    DofMap dofs_;
    std::map<std::pair<std::int32_t, int>, std::int32_t> dofIndex_;
    struct Frame {
        int step = 0;  ///< 0-based original step
        double stepTime = 0, totalTime = 0, dt = 0;
        int abqStep = 0, abqInc = 0;
        bool nlgeom = false;
    };
    std::vector<Frame> frames_;  ///< index 0 = initial state
    std::map<std::pair<int, int>, std::vector<abq::EmoBlock>> emo_;
    std::vector<std::tuple<std::int64_t, int, int>> pointKeys_;  ///< (element label, ip, sp) in layout order
    std::map<std::tuple<std::int64_t, int, int>, std::int32_t> pointIndex_;
    std::vector<FieldDescriptor> fields_;
    int nU_ = 0;
    size_t nS_ = 0, nE_ = 0, nSdv_ = 0;
    std::string strainKey_ = "E";
    bool hasLoads_[3] = {false, false, false};  // CLOAD, DLOAD, DSLOAD
    std::vector<std::string> elsetLines_;
    std::unique_ptr<std::counting_semaphore<64>> jobs_;
    mutable std::mutex logMutex_;
    std::atomic<int> branchCounter_{0};
    std::atomic<long long> jobCount_{0};
};

AbaqusOracle::AbaqusOracle(AbaqusRunConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.abaqusCommand.empty()) throw std::runtime_error("Abaqus command not configured");
    fs::create_directories(cfg_.scratchDir);
    deck_ = std::make_unique<inp::Deck>(inp::Deck::load(cfg_.inpPath));
    for (const auto& b : deck_->blocks()) {
        static const std::set<std::string> refused = {"*MPC", "*EQUATION", "*TIE", "*COUPLING", "*KINEMATIC COUPLING",
                                                      "*CONTACT PAIR", "*CONTACT", "*RIGID BODY", "*CONTACT INCLUSIONS"};
        if (refused.count(b.keyword))
            throw std::runtime_error(b.keyword + " (line " + std::to_string(b.line) +
                                     ") is not supported by the Abaqus sensitivity backend yet: constraint/contact residual "
                                     "semantics are not validated (docs/KNOWN_LIMITATIONS.md).");
        if (b.keyword == "*PART" || b.keyword == "*INSTANCE")
            throw std::runtime_error("This version of the Abaqus backend needs a flat input file (no *PART/*INSTANCE). "
                                     "Export a flat input (Abaqus/CAE: Model attributes > do not use parts and assemblies).");
        if (b.keyword == "*CLOAD") hasLoads_[0] = true;
        if (b.keyword == "*DLOAD") hasLoads_[1] = true;
        if (b.keyword == "*DSLOAD") hasLoads_[2] = true;
        if (b.keyword == "*DEPVAR") nSdv_ = 1;  // refined from results
    }
    if (deck_->steps().empty()) throw std::runtime_error("The input file has no *STEP");
    for (const auto& st : deck_->steps())
        if (deck_->blocks().at(static_cast<size_t>(st.blockIndex)).fileIndex != 0)
            throw std::runtime_error("Steps defined inside *INCLUDE files are not supported by the Abaqus backend yet");
    if (deck_->usesUserMaterial() || deck_->usesUserElement())
        if (cfg_.userSubroutine.empty())
            throw std::runtime_error("This model uses a user subroutine (UMAT/UEL). Provide the compiled object/library "
                                     "(or source with a working Abaqus compiler) in the run settings.");
    abq::LoadEvaluator check(*deck_);  // throws on unsupported load/BC features
    jobs_ = std::make_unique<std::counting_semaphore<64>>(std::clamp(cfg_.maxConcurrentJobs, 1, 64));
    // Whole-model element set for *ELEMENT MATRIX OUTPUT.
    std::string line;
    int k = 0;
    for (const auto& e : deck_->mesh().elements) {
        line += (k ? ", " : "") + std::to_string(e.label);
        if (++k == 16) {
            elsetLines_.push_back(line);
            line.clear();
            k = 0;
        }
    }
    if (!line.empty()) elsetLines_.push_back(line);
}

OracleCapabilities AbaqusOracle::capabilities() const {
    OracleCapabilities c;
    c.backendName = "Abaqus/Standard via restart-replay residual oracle";
    c.synthetic = false;
    c.exactTangent = false;
    c.supportsHistory = true;
    c.canRunInParallel = cfg_.maxConcurrentJobs > 1;
    for (const auto& [k, v] : abaqusCapabilities()) c.notes.push_back(k + ": " + v);
    return c;
}

void AbaqusOracle::cleanup(const fs::path& dir, const std::string& job) const {
    if (cfg_.keepJobFiles) return;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto stem = e.path().stem().string();
        if (stem == job || stem.rfind(job + "_", 0) == 0) fs::remove(e.path(), ec);
    }
}

AbaqusOracle::Edited AbaqusOracle::editedDeck(const std::vector<double>& delta, const fs::path& dir,
                                              const std::string& name) const {
    std::vector<inp::TokenEdit> edits;
    for (size_t p = 0; p < delta.size() && p < cfg_.parameters.size(); ++p)
        if (delta[p] != 0.0) {
            auto e = inp::editsForParameter(cfg_.parameters[p], cfg_.parameters[p].nominal + delta[p]);
            edits.insert(edits.end(), e.begin(), e.end());
        }
    fs::create_directories(dir);
    Edited out;
    out.root = inp::writeEditedDeck(*deck_, edits, dir.string(), name);
    out.deck = std::make_unique<inp::Deck>(inp::Deck::load(out.root.string()));
    return out;
}

std::string AbaqusOracle::modelDataOf(const inp::Deck& d, const fs::path& editedRoot) const {
    const auto lines = splitLines(readText(editedRoot));
    const int firstStepLine = d.blocks().at(static_cast<size_t>(d.steps().front().blockIndex)).line;
    std::string md;
    for (int i = 0; i < firstStepLine - 1 && i < static_cast<int>(lines.size()); ++i) md += lines[static_cast<size_t>(i)] + "\n";
    md += "*ELSET, ELSET=ABSS_ALL\n";
    for (const auto& l : elsetLines_) md += l + "\n";
    md += "*FILE FORMAT, ASCII\n";
    return md;
}

std::string AbaqusOracle::fieldRequests() const {
    std::string s = "*NODE FILE, FREQUENCY=1\nU, RF, CF\n";
    if (!deck_->usesUserElement()) {
        s += "*EL FILE, FREQUENCY=1\nS, " + strainKey_ + ", PE";
        if (nSdv_) s += ", SDV";
        s += "\n";
    }
    return s;
}

abq::FilIncrement AbaqusOracle::runJob(const fs::path& dir, const std::string& job, const std::string& oldjob,
                                       bool expectSingle, double expectTotal, std::vector<abq::FilIncrement>* all) {
    std::vector<std::string> args = {"job=" + job, "interactive", "ask_delete=OFF", "cpus=" + std::to_string(cfg_.cpus)};
    if (!oldjob.empty()) args.push_back("oldjob=" + oldjob);
    if (!cfg_.userSubroutine.empty()) args.push_back("user=" + cfg_.userSubroutine);
    jobs_->acquire();
    detail::ProcessResult res;
    try {
        res = detail::runProcess(cfg_.abaqusCommand, args, dir.string(), 3600, cfg_.cancel);
    } catch (...) {
        jobs_->release();
        throw;
    }
    jobs_->release();
    ++jobCount_;
    if (res.cancelled) throw std::runtime_error("Cancelled");
    if (res.timedOut) throw std::runtime_error("Abaqus job '" + job + "' exceeded the time limit and was stopped");
    const std::string dat = readText(dir / (job + ".dat")) + readText(dir / (job + ".msg"));
    if (res.exitCode != 0 || res.output.find("Abaqus JOB " + job + " COMPLETED") == std::string::npos ||
        dat.find("***ERROR") != std::string::npos) {
        std::string why = dat.substr(dat.find("***ERROR") == std::string::npos ? 0 : dat.find("***ERROR"), 600);
        if (why.empty()) why = res.output.substr(res.output.size() > 600 ? res.output.size() - 600 : 0);
        throw std::runtime_error("Abaqus job '" + job + "' did not complete. " + why);
    }
    if (dat.find("KEYWORD IS IGNORED IN A RESTART") != std::string::npos)
        throw std::runtime_error("Abaqus ignored a model keyword in restart job '" + job + "' (would silently change the model)");
    auto incs = abq::readFil((dir / (job + ".fil")).string());
    if (incs.empty()) throw std::runtime_error("No results in " + job + ".fil");
    if (expectSingle) {
        // A restart job also re-writes the restarted increment; keep only the increments of the new (last) step.
        const int lastStep = incs.back().step;
        incs.erase(std::remove_if(incs.begin(), incs.end(), [&](const abq::FilIncrement& x) { return x.step != lastStep; }),
                   incs.end());
        if (incs.size() != 1 || std::abs(incs[0].totalTime - expectTotal) > 1e-9 * std::max(1.0, std::abs(expectTotal)))
            throw std::runtime_error("Oracle increment in job '" + job + "' was subdivided or mistimed (" +
                                     std::to_string(incs.size()) + " increments); the residual would not be the "
                                     "single-increment map. Refusing to use it.");
    }
    if (all) *all = incs;
    return incs.back();
}

std::vector<double> AbaqusOracle::dofVector(const std::map<std::int64_t, std::vector<double>>& m) const {
    std::vector<double> v(static_cast<size_t>(dofs_.size()), 0.0);
    const auto& mesh = deck_->mesh();
    for (std::int32_t k = 0; k < dofs_.size(); ++k) {
        const auto& d = dofs_.dofs[static_cast<size_t>(k)];
        auto it = m.find(mesh.nodeKeys[static_cast<size_t>(d.node)].label);
        if (it != m.end() && static_cast<size_t>(d.dof) <= it->second.size()) v[static_cast<size_t>(k)] = it->second[static_cast<size_t>(d.dof - 1)];
    }
    return v;
}

FrameFields AbaqusOracle::fieldsFrom(const abq::FilIncrement& inc) const {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto& mesh = deck_->mesh();
    const size_t nn = static_cast<size_t>(mesh.nodeCount());
    const size_t np = pointKeys_.size();
    FrameFields f;
    auto nodal = [&](const std::map<std::int64_t, std::vector<double>>& m) {
        std::vector<double> v(nn * static_cast<size_t>(nU_), 0.0);
        for (size_t n = 0; n < nn; ++n)
            if (auto it = m.find(mesh.nodeKeys[n].label); it != m.end())
                for (int c = 0; c < nU_ && static_cast<size_t>(c) < it->second.size(); ++c) v[n * static_cast<size_t>(nU_) + static_cast<size_t>(c)] = it->second[static_cast<size_t>(c)];
        return v;
    };
    f.values.push_back(nodal(inc.U));
    f.values.push_back(nodal(inc.RF));
    if (np) {
        std::vector<double> S(np * nS_, nan), E(np * nE_, nan), PEEQ(np, nan), MIS(np, nan), SDV(np * std::max<size_t>(nSdv_, 1), nan);
        for (const auto& p : inc.points) {
            auto it = pointIndex_.find({p.element, p.ip, p.sectionPoint});
            if (it == pointIndex_.end()) continue;
            const size_t q = static_cast<size_t>(it->second);
            if (auto s = p.values.find("S"); s != p.values.end()) {
                for (size_t c = 0; c < nS_ && c < s->second.size(); ++c) S[q * nS_ + c] = s->second[c];
                MIS[q] = mises(fields_[2].components, s->second);
            }
            if (auto e = p.values.find(strainKey_); e != p.values.end())
                for (size_t c = 0; c < nE_ && c < e->second.size(); ++c) E[q * nE_ + c] = e->second[c];
            if (auto pe = p.values.find("PE"); pe != p.values.end() && !pe->second.empty()) PEEQ[q] = pe->second.back();
            if (nSdv_)
                if (auto sd = p.values.find("SDV"); sd != p.values.end())
                    for (size_t c = 0; c < nSdv_ && c < sd->second.size(); ++c) SDV[q * nSdv_ + c] = sd->second[c];
        }
        f.values.push_back(S);
        f.values.push_back(E);
        f.values.push_back(PEEQ);
        f.values.push_back(MIS);
        if (nSdv_) f.values.push_back(SDV);
    }
    return f;
}

std::vector<std::vector<double>> AbaqusOracle::runNominal(ResultStore& store, const ProgressFn& progress) {
    const fs::path dir = fs::path(cfg_.scratchDir) / "nominal";
    fs::create_directories(dir);
    if (progress) progress("Preparing nominal Abaqus job", 0.0);
    for (const auto& st : deck_->steps())
        if (st.nlgeom) strainKey_ = "LE";
    // Copy the user's deck (never modified) and add result requests.
    auto ed = editedDeck({}, dir, "abss_nominal_src");
    const auto lines = splitLines(readText(ed.root));
    std::map<int, std::string> before;  // 1-based line -> text inserted before it
    const auto& blocks = ed.deck->blocks();
    before[blocks.at(static_cast<size_t>(ed.deck->steps().front().blockIndex)).line] +=
        "*ELSET, ELSET=ABSS_ALL\n" + [&] {
            std::string s;
            for (const auto& l : elsetLines_) s += l + "\n";
            return s;
        }() + "*FILE FORMAT, ASCII\n";
    for (const auto& st : ed.deck->steps())
        before[blocks.at(static_cast<size_t>(st.endBlockIndex)).line] +=
            fieldRequests() +
            "*ELEMENT MATRIX OUTPUT, ELSET=ABSS_ALL, STIFFNESS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=abss_emo, FREQUENCY=1\n";
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (auto it = before.find(static_cast<int>(i + 1)); it != before.end()) text += it->second;
        text += lines[i] + "\n";
    }
    writeText(dir / "abss_nominal.inp", text);
    log("Nominal Abaqus job: " + (dir / "abss_nominal.inp").string());
    if (progress) progress("Running nominal Abaqus job", 0.1);
    std::vector<abq::FilIncrement> incs;
    runJob(dir, "abss_nominal", "", false, 0, &incs);
    emo_ = abq::readEmo((dir / "abss_emo.mtx").string());

    // Frames: initial state + every accepted increment (the frozen grid).
    frames_.clear();
    frames_.push_back({});
    std::map<int, int> abqStepToIdx;
    int s0 = -1, lastAbqStep = -1;
    for (const auto& inc : incs) {
        if (inc.step != lastAbqStep) {
            ++s0;
            lastAbqStep = inc.step;
        }
        Frame f;
        f.step = s0;
        f.abqStep = inc.step;
        f.abqInc = inc.increment;
        f.stepTime = inc.stepTime;
        f.totalTime = inc.totalTime;
        f.dt = inc.totalTime - frames_.back().totalTime;
        f.nlgeom = deck_->steps().at(static_cast<size_t>(s0)).nlgeom;
        frames_.push_back(f);
    }
    // DOF map: all DOFs present in the element matrices, plus constrained DOFs.
    const auto& mesh = deck_->mesh();
    std::map<std::int64_t, std::int32_t> byLabel;
    for (std::int32_t n = 0; n < mesh.nodeCount(); ++n) byLabel[mesh.nodeKeys[static_cast<size_t>(n)].label] = n;
    std::set<std::pair<std::int32_t, int>> dofSet;
    const auto& firstEmo = emo_.at({frames_[1].abqStep, frames_[1].abqInc});
    for (const auto& b : firstEmo)
        for (auto lab : b.nodes)
            for (int d : b.dofs) dofSet.insert({byLabel.at(lab), d});
    abq::LoadEvaluator ev(*deck_);
    std::vector<std::pair<std::int32_t, int>> cset;
    int inactive = 0;
    for (const auto& c : ev.prescribedDofs()) {
        if (dofSet.count(c)) cset.push_back(c);
        else ++inactive;  // e.g. PINNED/ENCASTRE DOF 3 on a 2D node: Abaqus ignores inactive DOFs too
    }
    if (inactive) log(std::to_string(inactive) + " boundary-condition DOF(s) are not active on any element and are ignored (as Abaqus does).");
    dofs_ = DofMap{};
    dofIndex_.clear();
    for (const auto& [n, d] : dofSet) {
        dofIndex_[{n, d}] = dofs_.size();
        dofs_.dofs.push_back({n, static_cast<std::int8_t>(d)});
        dofs_.prescribed.push_back(std::binary_search(cset.begin(), cset.end(), std::make_pair(n, d)) ? 1 : 0);
    }
    if (dofs_.freeIndices().empty()) throw std::runtime_error("All DOFs are constrained; nothing to differentiate");

    // Self-check: re-evaluated BCs and concentrated loads must reproduce the nominal job's own output.
    double worstBc = 0, worstCf = 0;
    for (size_t i = 1; i < frames_.size(); ++i) {
        const auto st = ev.evaluate(frames_[i].step, frames_[i].stepTime, frames_[i].totalTime);
        const auto& inc = incs[i - 1];
        for (const auto& [nd, v] : st.bc) {
            auto it = inc.U.find(mesh.nodeKeys[static_cast<size_t>(nd.first)].label);
            if (it == inc.U.end() || static_cast<size_t>(nd.second) > it->second.size()) continue;
            worstBc = std::max(worstBc, std::abs(it->second[static_cast<size_t>(nd.second - 1)] - v) / (1e-12 + std::abs(v) + 1e-9));
        }
        // CF reported by Abaqus where ABSS expects no concentrated load is a mismatch too (two-directional check).
        for (const auto& [lab, comps] : inc.CF)
            for (size_t c = 0; c < comps.size(); ++c)
                if (comps[c] != 0.0 && byLabel.count(lab) &&
                    !st.concentrated.count({byLabel.at(lab), static_cast<int>(c + 1)}))
                    worstCf = std::max(worstCf, 1.0);
        for (const auto& [nd, v] : st.concentrated) {
            auto it = inc.CF.find(mesh.nodeKeys[static_cast<size_t>(nd.first)].label);
            const double got = (it == inc.CF.end() || static_cast<size_t>(nd.second) > it->second.size()) ? 0.0 : it->second[static_cast<size_t>(nd.second - 1)];
            worstCf = std::max(worstCf, std::abs(got - v) / (1e-9 + std::abs(v)));
        }
    }
    log("Load/BC replay self-check vs nominal output: max BC mismatch " + std::to_string(worstBc) +
        ", max CLOAD mismatch " + std::to_string(worstCf));
    if (worstBc > 1e-6 || worstCf > 1e-6)
        throw std::runtime_error("ABSS could not reproduce this model's load/BC history exactly (mismatch " +
                                 std::to_string(std::max(worstBc, worstCf)) + "); the oracle would be wrong. "
                                 "Unsupported amplitude/step semantics.");

    // Material-point layout from the first increment.
    pointKeys_.clear();
    pointIndex_.clear();
    std::map<std::int64_t, std::vector<std::tuple<std::int64_t, int, int>>> perElem;
    for (const auto& p : incs.front().points) {
        perElem[p.element].emplace_back(p.element, p.ip, p.sectionPoint);
        if (auto s = p.values.find("S"); s != p.values.end()) nS_ = std::max(nS_, s->second.size());
        if (auto e = p.values.find(strainKey_); e != p.values.end()) nE_ = std::max(nE_, e->second.size());
        if (auto sd = p.values.find("SDV"); sd != p.values.end()) nSdv_ = std::max(nSdv_, sd->second.size());
    }
    PointLayout layout;
    layout.pointOffset.push_back(0);
    for (const auto& el : mesh.elements) {
        for (const auto& key : perElem[el.label]) {
            pointIndex_[key] = static_cast<std::int32_t>(pointKeys_.size());
            pointKeys_.push_back(key);
            layout.ipNumber.push_back(static_cast<std::int16_t>(std::get<1>(key)));
            layout.sectionPoint.push_back(static_cast<std::int16_t>(std::get<2>(key)));
        }
        layout.pointOffset.push_back(static_cast<std::int32_t>(pointKeys_.size()));
    }
    for (const auto& [lab, v] : incs.front().U) nU_ = std::max(nU_, static_cast<int>(v.size()));

    // Result store.
    store.title = deck_->heading().empty() ? fs::path(cfg_.inpPath).filename().string() : deck_->heading();
    store.sourceInp = cfg_.inpPath;
    store.backend = "abaqus";
    store.synthetic = false;
    store.mesh = mesh;
    store.points = layout;
    std::vector<std::string> uc, rc;
    for (int c = 0; c < nU_; ++c) {
        uc.push_back(c < 3 ? "U" + std::to_string(c + 1) : "UR" + std::to_string(c - 2));
        rc.push_back(c < 3 ? "RF" + std::to_string(c + 1) : "RM" + std::to_string(c - 2));
    }
    fields_ = {{"U", FieldLocation::Node, FieldKind::Vector, uc, "Displacement", ""},
               {"RF", FieldLocation::Node, FieldKind::Vector, rc, "Reaction force", ""}};
    if (!pointKeys_.empty()) {
        fields_.push_back({"S", FieldLocation::IntegrationPoint, FieldKind::SymTensor, tensorNames("S", nS_), "Stress", ""});
        fields_.push_back({strainKey_, FieldLocation::IntegrationPoint, FieldKind::SymTensor, tensorNames(strainKey_, nE_),
                           strainKey_ == "LE" ? "Logarithmic strain" : "Total strain", ""});
        fields_.push_back({"PEEQ", FieldLocation::IntegrationPoint, FieldKind::Scalar, {"PEEQ"}, "Equivalent plastic strain", ""});
        fields_.push_back({"S_Mises", FieldLocation::IntegrationPoint, FieldKind::Scalar, {"Mises"}, "von Mises stress (invariant)", ""});
        if (nSdv_) {
            std::vector<std::string> sc;
            for (size_t k = 0; k < nSdv_; ++k) sc.push_back("SDV" + std::to_string(k + 1));
            fields_.push_back({"SDV", FieldLocation::IntegrationPoint, FieldKind::Generic, sc, "Solution-dependent state variables", ""});
        }
    }
    store.fields = fields_;
    store.parameters = cfg_.parameters;
    store.frames.clear();
    store.frames.push_back({1, deck_->steps().front().name, 0, 0.0, 0.0});
    for (size_t i = 1; i < frames_.size(); ++i)
        store.frames.push_back({frames_[i].step + 1, deck_->steps().at(static_cast<size_t>(frames_[i].step)).name,
                                frames_[i].abqInc, frames_[i].stepTime, frames_[i].totalTime});
    store.clearSeries();
    std::vector<std::vector<double>> us;
    us.push_back(std::vector<double>(static_cast<size_t>(dofs_.size()), 0.0));
    {
        abq::FilIncrement zero;
        auto f0 = fieldsFrom(zero);
        for (auto& v : f0.values)
            for (double& x : v)
                if (std::isnan(x)) x = 0.0;
        for (size_t k = 0; k < fields_.size(); ++k) store.set(static_cast<int>(k), DerivativeSpec::nominal(), 0, f0.values[k]);
    }
    for (size_t i = 0; i < incs.size(); ++i) {
        us.push_back(dofVector(incs[i].U));
        const auto f = fieldsFrom(incs[i]);
        for (size_t k = 0; k < fields_.size(); ++k) store.set(static_cast<int>(k), DerivativeSpec::nominal(), static_cast<int>(i + 1), f.values[k]);
    }
    store.log.push_back("Nominal Abaqus analysis: " + std::to_string(incs.size()) + " accepted increments, " +
                        std::to_string(dofs_.size()) + " DOFs (" + std::to_string(dofs_.freeIndices().size()) + " free).");
    if (progress) progress("Nominal Abaqus job finished", 1.0);
    return us;
}

CsrMatrix AbaqusOracle::tangent(int frame) {
    const auto& f = frames_.at(static_cast<size_t>(frame));
    auto it = emo_.find({f.abqStep, f.abqInc});
    if (it == emo_.end()) throw std::runtime_error("No element matrices for increment " + std::to_string(frame));
    const auto& mesh = deck_->mesh();
    std::map<std::int64_t, std::int32_t> byLabel;
    for (std::int32_t n = 0; n < mesh.nodeCount(); ++n) byLabel[mesh.nodeKeys[static_cast<size_t>(n)].label] = n;
    std::vector<Triplet> t;
    for (const auto& b : it->second) {
        std::vector<std::int32_t> idx;
        for (auto lab : b.nodes)
            for (int d : b.dofs) idx.push_back(dofIndex_.at({byLabel.at(lab), d}));
        const size_t n = idx.size();
        for (size_t r = 0; r < n; ++r)
            for (size_t c = 0; c < n; ++c)
                if (b.K[r * n + c] != 0.0) t.push_back({idx[r], idx[c], b.K[r * n + c]});
    }
    return CsrMatrix::fromTriplets(dofs_.size(), dofs_.size(), std::move(t));
}

std::unique_ptr<HistoryBranch> AbaqusOracle::openBranch(const std::vector<double>& delta) {
    return std::make_unique<AbaqusBranch>(*this, delta, branchCounter_++);
}

std::vector<std::vector<double>> AbaqusOracle::solvePerturbed(const std::vector<double>& delta, std::vector<FrameFields>* fields) {
    // Full re-equilibrated reference on the frozen nominal grid (requires a uniform increment per step).
    const int id = branchCounter_++;
    const fs::path dir = fs::path(cfg_.scratchDir) / ("rerun_" + std::to_string(id));
    auto ed = editedDeck(delta, dir, "abss_rerun_src");
    const auto lines = splitLines(readText(ed.root));
    const auto& blocks = ed.deck->blocks();
    std::map<int, std::string> before;
    std::map<int, std::string> replace;
    before[blocks.at(static_cast<size_t>(ed.deck->steps().front().blockIndex)).line] += "*FILE FORMAT, ASCII\n";
    for (size_t s = 0; s < ed.deck->steps().size(); ++s) {
        const auto& st = ed.deck->steps()[s];
        std::vector<double> dts;
        for (size_t i = 1; i < frames_.size(); ++i)
            if (frames_[i].step == static_cast<int>(s)) dts.push_back(frames_[i].dt);
        if (dts.empty()) return {};
        for (double d : dts)
            if (std::abs(d - dts.front()) > 1e-9 * dts.front()) {
                log("Validation rerun skipped: increments of step " + std::to_string(s + 1) + " are not uniform, so a rerun "
                    "cannot reproduce the nominal time grid.");
                return {};
            }
        // Curie E2: default extrapolation diverged at an unloading kink with fixed increments -> EXTRAPOLATION=NO.
        {
            const auto& sb = blocks.at(static_cast<size_t>(st.blockIndex));
            if (!sb.hasParam("EXTRAPOLATION") && sb.keywordLineCount == 1)
                replace[sb.line] = lines.at(static_cast<size_t>(sb.line - 1)) + ", EXTRAPOLATION=NO";
        }
        // find the *STATIC block of this step
        for (int bi = st.blockIndex; bi <= st.endBlockIndex; ++bi) {
            const auto& b = blocks.at(static_cast<size_t>(bi));
            if (b.keyword != "*STATIC") continue;
            replace[b.line] = "*STATIC, DIRECT";
            for (int c = 1; c < b.keywordLineCount; ++c) replace[b.line + c] = "**";
            const std::string dt = inp::formatNumber(dts.front());
            if (!b.data.empty()) replace[b.data.front().lineNumber] = dt + ", " + inp::formatNumber(st.timePeriod);
            else before[b.line + b.keywordLineCount] += dt + ", " + inp::formatNumber(st.timePeriod) + "\n";
        }
        // Reference reruns: tighten the residual tolerance (Curie/Gauss: rerun noise ~ tol/h^k dominates validation).
        before[blocks.at(static_cast<size_t>(st.endBlockIndex)).line] +=
            fieldRequests() + "*CONTROLS, PARAMETERS=FIELD, FIELD=GLOBAL\n1.E-8, 1.E-6\n";
    }
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        const int ln = static_cast<int>(i + 1);
        if (auto it = before.find(ln); it != before.end()) text += it->second;
        if (auto it = replace.find(ln); it != replace.end()) text += it->second + "\n";
        else text += lines[i] + "\n";
    }
    writeText(dir / "abss_rerun.inp", text);
    std::vector<abq::FilIncrement> incs;
    try {
        runJob(dir, "abss_rerun", "", false, 0, &incs);
    } catch (const std::exception& e) {
        log(std::string("Validation rerun failed: ") + e.what());
        return {};
    }
    if (incs.size() + 1 != frames_.size()) return {};
    for (size_t i = 0; i < incs.size(); ++i)
        if (std::abs(incs[i].totalTime - frames_[i + 1].totalTime) > 1e-9 * std::max(1.0, frames_[i + 1].totalTime)) return {};
    std::vector<std::vector<double>> us;
    us.push_back(std::vector<double>(static_cast<size_t>(dofs_.size()), 0.0));
    if (fields) {
        fields->clear();
        abq::FilIncrement zero;
        auto f0 = fieldsFrom(zero);
        for (auto& v : f0.values)
            for (double& x : v)
                if (std::isnan(x)) x = 0.0;
        fields->push_back(f0);
    }
    for (const auto& inc : incs) {
        us.push_back(dofVector(inc.U));
        if (fields) fields->push_back(fieldsFrom(inc));
    }
    cleanup(dir, "abss_rerun");
    return us;
}

// ---------------------------------------------------------------------------------------------------------------
AbaqusBranch::AbaqusBranch(AbaqusOracle& o, std::vector<double> delta, int id) : o_(o), delta_(std::move(delta)), id_(id) {
    dir_ = fs::path(o_.cfg_.scratchDir) / ("branch_" + std::to_string(id_));
    auto ed = o_.editedDeck(delta_, dir_, "abss_branch_src");
    modelData_ = o_.modelDataOf(*ed.deck, ed.root);
    deck_ = std::move(ed.deck);
    loads_ = std::make_unique<abq::LoadEvaluator>(*deck_);
}

AbaqusBranch::~AbaqusBranch() {
    if (!o_.cfg_.keepJobFiles) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
}

std::string AbaqusBranch::stepText(int frame, const std::vector<double>& u, bool commit) const {
    const auto& f = o_.frames_.at(static_cast<size_t>(frame));
    const auto st = loads_->evaluate(f.step, f.stepTime, f.totalTime);
    const auto& mesh = o_.deck_->mesh();
    std::ostringstream os;
    os << "*STEP, NAME=ABSS_INC_" << frame << ", NLGEOM=" << (f.nlgeom ? "YES" : "NO") << ", INC=1\n";
    os << "*STATIC, DIRECT\n" << inp::formatNumber(f.dt) << ", " << inp::formatNumber(f.dt) << "\n";
    if (!commit && frame > 1) os << "*RESTART, WRITE, FREQUENCY=0\n";
    os << "*BOUNDARY, OP=NEW\n";
    const auto& dm = o_.dofs_;
    for (std::int32_t k = 0; k < dm.size(); ++k) {
        const auto& d = dm.dofs[static_cast<size_t>(k)];
        double v = u.at(static_cast<size_t>(k));
        if (dm.prescribed[static_cast<size_t>(k)]) {
            auto it = st.bc.find({d.node, d.dof});
            v = it == st.bc.end() ? 0.0 : it->second;
        }
        os << mesh.nodeKeys[static_cast<size_t>(d.node)].label << ", " << int(d.dof) << ", " << int(d.dof) << ", "
           << inp::formatNumber(v) << "\n";
    }
    const char* kws[] = {"*CLOAD", "*DLOAD", "*DSLOAD"};
    for (int k = 0; k < 3; ++k) {
        if (!o_.hasLoads_[k]) continue;
        os << kws[k] << ", OP=NEW\n";
        bool any = false;
        for (const auto& l : st.loads)
            if (l.keyword == kws[k]) {
                for (size_t c = 0; c < l.fields.size(); ++c) os << (c ? ", " : "") << l.fields[c];
                os << "\n";
                any = true;
            }
        if (!any && k == 0) os << mesh.nodeKeys[static_cast<size_t>(dm.dofs[0].node)].label << ", " << int(dm.dofs[0].dof) << ", 0.\n";
    }
    if (commit) os << o_.fieldRequests();
    else os << "*NODE FILE, FREQUENCY=1\nU, RF\n";
    os << "*END STEP\n";
    return os.str();
}

abq::FilIncrement AbaqusBranch::runIncrement(int frame, const std::vector<double>& u, bool commit, const std::string& job) {
    std::string text;
    if (frame == 1) {
        text = modelData_;
        if (commit) text += "*RESTART, WRITE, FREQUENCY=1\n";
    } else {
        text = "*HEADING\nABSS history-branch increment\n*RESTART, READ, STEP=" + std::to_string(frame - 1) + ", INC=1, END STEP\n";
    }
    text += stepText(frame, u, commit);
    writeText(dir_ / (job + ".inp"), text);
    return o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, true, o_.frames_.at(static_cast<size_t>(frame)).totalTime);
}

std::vector<std::vector<double>> AbaqusBranch::residuals(int frame, const std::vector<std::vector<double>>& us) {
    if (frame != committed_ + 1) throw std::logic_error("Abaqus branch: probe out of sequence");
    std::vector<std::vector<double>> out(us.size());
    std::vector<std::future<void>> fut;
    for (size_t k = 0; k < us.size(); ++k)
        fut.push_back(std::async(std::launch::async, [&, k] {
            const std::string job = "b" + std::to_string(id_) + "_p" + std::to_string(frame) + "_" + std::to_string(probeCounter_++);
            const auto inc = runIncrement(frame, us[k], false, job);
            auto rf = o_.dofVector(inc.RF);
            for (double& x : rf) x = -x;  // r = F_ext - F_int = -RF on every held DOF
            out[k] = std::move(rf);
            o_.cleanup(dir_, job);
        }));
    for (auto& f : fut) f.get();
    return out;
}

FrameFields AbaqusBranch::commit(int frame, const std::vector<double>& u, std::vector<double>* committedU) {
    if (frame != committed_ + 1) throw std::logic_error("Abaqus branch: commit out of sequence");
    const std::string job = "b" + std::to_string(id_) + "_c" + std::to_string(frame);
    const auto inc = runIncrement(frame, u, true, job);
    lastResidual_ = o_.dofVector(inc.RF);
    for (double& x : lastResidual_) x = -x;
    if (committedU) *committedU = o_.dofVector(inc.U);
    // Earlier commit jobs are still referenced by Abaqus restart chains; they are removed with the branch directory.
    lastCommitJob_ = job;
    committed_ = frame;
    return o_.fieldsFrom(inc);
}

}  // namespace

std::unique_ptr<Oracle> makeAbaqusOracle(const AbaqusRunConfig& cfg) { return std::make_unique<AbaqusOracle>(cfg); }

}  // namespace abss
