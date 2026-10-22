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
#include "abaqus_replica.hpp"
#include "abss/core/profile.hpp"
#include "abss/inp/deck.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
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
        {"Model structure", "Flat decks and part/assembly/instance decks (numbering verified against Abaqus's model records)"},
        {"Constraints", "*TIE, *EQUATION, *MPC, couplings, rigid bodies: dependent DOFs identified from Abaqus (DOF check job) and computed by Abaqus"},
        {"Contact", "Contact forces enter -RF; derivatives hold while the contact status is fixed (status changes are flagged as non-smooth)"},
        {"UMAT/UEL", "Source, .obj or prebuilt library (opaque PROPS); verified on Abaqus 2024 at order 1"},
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

/// Tight equilibrium controls for the nominal job and validation reruns. R_n = 1e-8 alone makes Abaqus judge
/// round-off-level residual fluctuations as divergence and cut back (measured: a shell model went from 14 to 41
/// increments), so the iteration limits are relaxed too (I_0 8, I_R 20, I_C 30, I_L 20, I_G 8, I_S 30): with them the
/// same model takes 13 increments.
const char* kTightControls =
    "*CONTROLS, PARAMETERS=FIELD, FIELD=GLOBAL\n1.E-8, 1.E-6\n"
    "*CONTROLS, PARAMETERS=TIME INCREMENTATION\n8, 20, 9, 30, 20, 8, 30, 5\n";

/// Split one Abaqus job's wall time into phases from the timestamps its driver prints (1 s resolution):
/// driver start-up + licence checkout (until the input file processor begins), input processing (pre.exe),
/// analysis (standard.exe, including its setup), and wrap-up (results-database conversion until process exit).
void recordAbaqusPhases(const std::string& out, std::chrono::system_clock::time_point start, double procWall) {
    auto stampAfter = [&](const char* marker) -> std::optional<std::time_t> {
        const auto p = out.find(marker);
        if (p == std::string::npos) return std::nullopt;
        const auto nl = out.find('\n', p);
        if (nl == std::string::npos) return std::nullopt;
        const auto e = out.find('\n', nl + 1);
        const std::string line = out.substr(nl + 1, e == std::string::npos ? std::string::npos : e - nl - 1);
        // "%m/%d/%Y %I:%M:%S %p", parsed by hand: libstdc++'s std::get_time ignores %p (PM times came out 12 h early).
        int mo = 0, dd = 0, yy = 0, hh = 0, mi = 0, ss = 0;
        char ampm[3] = {0, 0, 0};
        if (std::sscanf(line.c_str(), "%d/%d/%d %d:%d:%d %2s", &mo, &dd, &yy, &hh, &mi, &ss, ampm) != 7) return std::nullopt;
        const bool pm = ampm[0] == 'P' || ampm[0] == 'p';
        if (hh == 12) hh = pm ? 12 : 0;
        else if (pm) hh += 12;
        std::tm tm{};
        tm.tm_year = yy - 1900, tm.tm_mon = mo - 1, tm.tm_mday = dd, tm.tm_hour = hh, tm.tm_min = mi, tm.tm_sec = ss;
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    };
    const auto tPre = stampAfter("Begin Analysis Input File Processor");
    const auto tStd = stampAfter("Begin Abaqus/Standard Analysis");
    const auto tWrap = stampAfter("Begin SIM Wrap-up");
    if (!tPre || !tStd) return;
    const double t0 = static_cast<double>(std::chrono::system_clock::to_time_t(start)) +
                      std::chrono::duration<double>(start - std::chrono::system_clock::from_time_t(std::chrono::system_clock::to_time_t(start))).count();
    const double tEnd = t0 + procWall;
    const double a = std::max(0.0, static_cast<double>(*tPre) - t0), b = std::max(0.0, static_cast<double>(*tStd - *tPre));
    const double c = tWrap ? std::max(0.0, static_cast<double>(*tWrap - *tStd)) : std::max(0.0, tEnd - static_cast<double>(*tStd));
    const double d = tWrap ? std::max(0.0, tEnd - static_cast<double>(*tWrap)) : 0.0;
    prof::add("job.phase.driver_and_licence", a);
    prof::add("job.phase.input_processing", b);
    prof::add("job.phase.analysis", c);
    prof::add("job.phase.wrapup", d);
}

/// Expand every *INCLUDE card recursively into one text (paths relative to the including file). Data-only INPUT=
/// options of other keywords (e.g. *NODE, INPUT=) stay as they are; they resolve next to the flattened file.
void inlineIncludes(const fs::path& file, std::string& out, int depth) {
    if (depth > 32) throw std::runtime_error("*INCLUDE nesting deeper than 32 levels (circular include?) at " + file.string());
    const auto lines = splitLines(readText(file));
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string up = lines[i];
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto first = up.find_first_not_of(" \t");
        if (first != std::string::npos && up.compare(first, 8, "*INCLUDE") == 0 && up.compare(first, 2, "**") != 0 &&
            up.find("PASSWORD") == std::string::npos) {
            const auto p = up.find("INPUT");
            const auto eq = p == std::string::npos ? std::string::npos : lines[i].find('=', p);
            if (eq != std::string::npos) {
                std::string v = lines[i].substr(eq + 1);
                const auto comma = v.find(',');
                if (!v.empty() && v.find_first_not_of(" \t") != std::string::npos && v[v.find_first_not_of(" \t")] == '"') {
                    v = v.substr(v.find('"') + 1);
                    v = v.substr(0, v.find('"'));
                } else if (comma != std::string::npos) {
                    v = v.substr(0, comma);
                }
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                fs::path inc(v);
                if (inc.is_relative()) inc = file.parent_path() / inc;
                out += "** ABSS: inlined *INCLUDE " + v + "\n";
                inlineIncludes(inc, out, depth + 1);
                continue;
            }
        }
        out += lines[i] + "\n";
    }
}

using Mat3 = std::array<std::array<double, 3>, 3>;
Mat3 rotExp(const std::array<double, 3>& p) {  // Rodrigues
    const double th = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const Mat3 K = {{{0, -p[2], p[1]}, {p[2], 0, -p[0]}, {-p[1], p[0], 0}}};
    const double a = th < 1e-14 ? 1.0 : std::sin(th) / th, b = th < 1e-14 ? 0.5 : (1 - std::cos(th)) / (th * th);
    Mat3 R{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double kk = 0;
            for (int m = 0; m < 3; ++m) kk += K[i][m] * K[m][j];
            R[i][j] = (i == j) + a * K[i][j] + b * kk;
        }
    return R;
}
Mat3 rotMul(const Mat3& A, const Mat3& B) {
    Mat3 C{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int m = 0; m < 3; ++m) C[i][j] += A[i][m] * B[m][j];
    return C;
}
Mat3 rotTranspose(const Mat3& A) {
    Mat3 T{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) T[i][j] = A[j][i];
    return T;
}
std::array<double, 3> rotLog(const Mat3& R) {
    const double c = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1.0) / 2.0, -1.0, 1.0), th = std::acos(c);
    const double f = th < 1e-12 ? 0.5 : th / (2.0 * std::sin(th));
    return {f * (R[2][1] - R[1][2]), f * (R[0][2] - R[2][0]), f * (R[1][0] - R[0][1])};
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
class ReplicaGroup;

// ---------------------------------------------------------------------------------------------------------------
class AbaqusBranch final : public HistoryBranch {
    friend class ReplicaGroup;
public:
    AbaqusBranch(AbaqusOracle& o, std::vector<double> delta, int id);
    ~AbaqusBranch() override;
    std::vector<std::vector<double>> residuals(int frame, const std::vector<std::vector<double>>& u) override;
    FrameFields commit(int frame, const std::vector<double>& u, std::vector<double>* committedU) override;
    std::vector<double> lastCommitResidual() const override { return lastResidual_; }
    bool canFuseNextProbe(int frame) const override;
    FrameFields commitAndProbe(int frame, const std::vector<double>& u, const std::vector<std::vector<double>>& uNext,
                               std::vector<std::vector<double>>& nextResiduals) override;

private:
    std::string stepText(int frame, const std::vector<double>& u, bool commit) const;
    /// One Abaqus job: increment `frame` at u (commit or probe); with uNext, a commit followed by a probe step of
    /// frame+1 (no restart data written for it), whose increment is returned in *nextInc.
    abq::FilIncrement runIncrement(int frame, const std::vector<double>& u, bool commit, const std::string& job,
                                   const std::vector<double>* uNext = nullptr, abq::FilIncrement* nextInc = nullptr);
    FrameFields finishCommit(int frame, const abq::FilIncrement& inc, std::vector<double> written, const std::string& job,
                             std::vector<double>* committedU);

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
    std::map<std::int32_t, double> releaseForce_;  ///< DOF -> this branch's reaction when its BC is released next step
    // Dynamic steps: this branch's own mass matrix (parameters may change it) and kinematic state at the start of the
    // next increment (u, v, a on the DOF map), integrated with the step's Newmark/HHT (or backward Euler) operator.
    void ensureDynamicState(int frame);
    std::vector<double> inertia(int frame, const std::vector<double>& u) const;
    std::string loadText(const abq::LoadState& st, const std::vector<std::string>& extraCload) const;
    /// Values to write in *BOUNDARY for every DOF of the job at `frame` (free: u, held: the BC history), with finite
    /// rotations converted to Abaqus's incremental convention (see rotNodes_).
    std::vector<double> bcValues(int frame, const std::vector<double>& u) const;
    std::vector<double> lastCommitU_;
    std::vector<double> lastCommitB_;  ///< *BOUNDARY values written by the last commit job (rotation bookkeeping)
    CsrMatrix mass_;
    bool massLoaded_ = false;
    std::vector<double> dynU_, dynV_, dynA_;
    int dynReadyFor_ = 0;  ///< frame whose increment-start state is in dynU_/dynV_/dynA_ (0 = none)
};

class AbaqusOracle final : public Oracle {
public:
    explicit AbaqusOracle(AbaqusRunConfig cfg);
    ~AbaqusOracle() override;
    void runWave(int frame, std::vector<WaveRequest>& wave, bool concurrent) override;
    bool batchesWaves() const override { return cfg_.batchBranches && !batchRefused_; }
    int replayProbesPerEvaluation() const override { return batchesWaves() ? kReplayReplicas : 1; }
    bool tangentPatternComplete(int) const override { return patternComplete_; }
    std::atomic<bool> patternComplete_{false};
    static constexpr int kReplayReplicas = 3;  ///< replay probe + tangent-check pair in one job
    /// Replicated-branch group holding branch b (groups are formed at the first branch evaluation), or nullptr when
    /// branches run as separate jobs (batching off or refused for this model). One group per job slot, so a wave
    /// keeps every slot busy; the replay branch and its replicas are in group 0.
    ReplicaGroup* group(const AbaqusBranch* b);
    bool formGroups();
    void forgetBranch(const AbaqusBranch* b);
    std::vector<AbaqusBranch*> openBranches_;
    std::mutex groupMutex_;
    std::vector<std::unique_ptr<ReplicaGroup>> groups_;
    std::map<const AbaqusBranch*, ReplicaGroup*> groupOf_;
    std::atomic<bool> batchRefused_{false};
    OracleCapabilities capabilities() const override;
    std::vector<std::vector<double>> runNominal(ResultStore& store, const ProgressFn& progress) override;
    const DofMap& dofMap() const override { return dofs_; }
    std::vector<std::int32_t> freeIndicesAt(int frame) const override {
        std::vector<std::int32_t> f;
        for (std::int32_t k = 0; k < dofs_.size(); ++k)
            if (!prescribedAt(frame, k)) f.push_back(k);
        return f;
    }
    /// DOF k is held by a boundary condition during the step of `frame`.
    bool prescribedAt(int frame, std::int32_t k) const {
        const int s = frames_.at(static_cast<size_t>(std::max(frame, 1))).step;
        return prescribedByStep_.at(static_cast<size_t>(s)).at(static_cast<size_t>(k)) != 0;
    }
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
    /// Nodes with all three rotational DOFs (4, 5, 6) on the DOF map: {DOF index of UR1, UR2, UR3}. Measured on Abaqus
    /// 2024 (NLGEOM): a rotation BC in a new step is applied as the increment b - b_prev of the BC value, composed in
    /// space, R = exp(b - b_prev) R_prev; output UR is the total rotation vector log(R). So reaching the total rotation
    /// phi needs b = b_prev + log(R(phi) R(phi_prev)^T).
    std::vector<std::array<std::int32_t, 3>> rotNodes_;
    std::vector<std::vector<std::uint8_t>> prescribedByStep_;  ///< [step][dof]: held by a BC during that step
    std::vector<std::vector<std::int32_t>> releasedByStep_;    ///< [step]: DOFs whose BC is removed at the step start
    struct Frame {
        int step = 0;  ///< 0-based original step
        double stepTime = 0, totalTime = 0, dt = 0;
        int abqStep = 0, abqInc = 0;
        bool nlgeom = false;
        bool dynamic = false;        ///< implicit *DYNAMIC step: residual = -RF - M a(u)
        bool backwardEuler = false;  ///< quasi-static dynamic application (backward Euler) instead of HHT/Newmark
        double alpha = 0, beta = 0.25, gamma = 0.5;
    };
    /// Each step's procedure keyword line as written in the deck (user parameters kept, e.g. STABILIZE, ALPHA),
    /// made fixed-increment (DIRECT; *VISCO without CETOL), for oracle steps and validation reruns.
    std::vector<std::string> procLine_;
    bool dynamic_ = false;
    CsrMatrix mass_;               ///< nominal mass matrix on the DOF map (dynamic models; tangent preconditioner)
    std::vector<double> v0_;       ///< initial velocity on the DOF map (*INITIAL CONDITIONS, TYPE=VELOCITY)
    CsrMatrix assembleMass(const std::map<std::pair<int, int>, std::vector<abq::EmoBlock>>& m) const;
    /// Acceleration and velocity at u for frame f from the state (un, vn, an) at the start of its increment.
    void kinematics(int frame, const std::vector<double>& u, const std::vector<double>& un, const std::vector<double>& vn,
                    const std::vector<double>& an, std::vector<double>& a, std::vector<double>* v) const;
    std::vector<Frame> frames_;  ///< index 0 = initial state
    std::map<std::pair<int, int>, std::vector<abq::EmoBlock>> emo_;
    bool emoAnyIncrement_ = false;  ///< element matrices from a separate job: use the nearest available increment
    const std::vector<abq::EmoBlock>& emoFor(int abqStep, int abqInc) const {
        if (auto it = emo_.find({abqStep, abqInc}); it != emo_.end()) return it->second;
        if (emoAnyIncrement_ && !emo_.empty()) {
            auto it = emo_.lower_bound({abqStep, abqInc});
            return it == emo_.end() ? std::prev(it)->second : it->second;
        }
        throw std::runtime_error("No element matrices for step " + std::to_string(abqStep) + " increment " + std::to_string(abqInc));
    }
    std::vector<std::tuple<std::int64_t, int, int>> pointKeys_;  ///< (element label, ip, sp) in layout order
    std::map<std::tuple<std::int64_t, int, int>, std::int32_t> pointIndex_;
    std::vector<FieldDescriptor> fields_;
    int nU_ = 0;
    size_t nS_ = 0, nE_ = 0, nSdv_ = 0;
    std::string strainKey_ = "E";
    /// Abaqus's node/element numbers in .fil/.mtx output <-> mesh indices. Flat deck: the labels. Part/instance deck:
    /// Abaqus's global internal numbers (instance blocks in deck order, labels ascending), verified against the
    /// model definition records of the nominal .fil before use.
    std::string nodeRef(std::int32_t n) const;
    void buildNumbering(const abq::FilModel& fm);
    std::int32_t meshNode(std::int64_t number) const;
    std::vector<std::int64_t> nodeNumber_, elemNumber_;
    std::map<std::int64_t, std::int32_t> filNode_;
    bool assembly_ = false;
    bool hasConstraints_ = false;  ///< ties, equations, MPCs, couplings, rigid bodies: dependent DOFs detected after the nominal job
    bool hasContact_ = false;      ///< contact: contact forces are part of -RF; status changes are non-smooth events
    bool hasLoads_[4] = {false, false, false, false};  // CLOAD, DLOAD, DSLOAD, TEMPERATURE (step data)
    bool hasExpansion_ = false;  ///< *EXPANSION present: element matrices come from a separate job (see runNominal)
    std::string elsetText_;  ///< *ELSET ABSS_ALL (whole model) for *ELEMENT MATRIX OUTPUT
    /// 1-based root-file line before which elsetText_ goes (*END ASSEMBLY, or the first *STEP of a flat deck).
    int elsetLineOf(const inp::Deck& d) const;
    std::string usubDir_;  ///< folder with standardU.dll loaded by every job (usub_lib_dir); empty = no user subroutine
    std::unique_ptr<std::counting_semaphore<64>> jobs_;
    mutable std::mutex logMutex_;
    std::atomic<int> branchCounter_{0};
    std::atomic<long long> jobCount_{0};
};

// ---------------------------------------------------------------------------------------------------------------
/// Replicated history branches (AbaqusRunConfig::batchBranches): every branch of the run is a disjoint copy of the
/// model in one Abaqus job and restart chain (abaqus_replica.hpp). The replay branch has 3 replicas (its probe and the
/// tangent-check pair fit in one job), every perturbed branch 1. A probe wave costs ceil(probes / replicas) jobs; a
/// commit wave costs one job that commits every copy and evaluates the next increment's probes in a second step.
class ReplicaGroup {
public:
    ReplicaGroup(AbaqusOracle& o, const std::vector<AbaqusBranch*>& members, int index);
    ~ReplicaGroup() {
        if (!o_.cfg_.keepJobFiles) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }
    bool contains(const AbaqusBranch* b) const { return copiesOf_.count(b) > 0; }
    bool closed = false;  ///< set when a member branch is destroyed
    std::vector<std::vector<double>> probe(int frame, AbaqusBranch* b, const std::vector<std::vector<double>>& us);
    void runWave(int frame, const std::vector<WaveRequest*>& wave);

private:
    struct Copy {
        AbaqusBranch* b;
        int replica;
    };
    using Request = std::map<const AbaqusBranch*, const std::vector<std::vector<double>>*>;
    std::map<const AbaqusBranch*, std::vector<std::vector<double>>> probeJobs(int frame, const Request& req);
    std::string header(int frame, bool commitWritesRestart) const;
    std::string step(int frame, const std::vector<const std::vector<double>*>& copyU, bool commit) const;
    const std::vector<double>& hold(const AbaqusBranch* b) const;
    void checkReplicas(const abq::FilIncrement& inc, const AbaqusBranch* b, const std::string& job) const;

    AbaqusOracle& o_;
    std::vector<Copy> copies_;
    std::map<const AbaqusBranch*, std::vector<int>> copiesOf_;  ///< branch -> copy index of each replica
    abq::ReplicaNumbering num_;
    std::string modelData_;
    fs::path dir_;
    std::string lastCommitJob_;
    int committed_ = 0;
    std::atomic<int> counter_{0};
    std::vector<double> zeros_;
};

AbaqusOracle::AbaqusOracle(AbaqusRunConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.abaqusCommand.empty()) throw std::runtime_error("Abaqus command not configured");
    fs::create_directories(cfg_.scratchDir);
    deck_ = std::make_unique<inp::Deck>(inp::Deck::load(cfg_.inpPath));
    for (const auto& b : deck_->blocks()) {
        // Constraints: dependent DOFs are identified from Abaqus itself after the nominal job (runNominal).
        static const std::set<std::string> constraints = {
            "*MPC", "*EQUATION", "*TIE", "*COUPLING", "*KINEMATIC COUPLING", "*DISTRIBUTING COUPLING", "*RIGID BODY",
            "*SHELL TO SOLID COUPLING", "*EMBEDDED ELEMENT", "*KINEMATIC", "*DISTRIBUTING"};
        static const std::set<std::string> contact = {"*CONTACT PAIR", "*CONTACT", "*CONTACT INCLUSIONS", "*SURFACE INTERACTION",
                                                      "*CONTACT PROPERTY ASSIGNMENT"};
        if (constraints.count(b.keyword)) hasConstraints_ = true;
        if (contact.count(b.keyword)) hasContact_ = true;
        if (b.keyword == "*ASSEMBLY" || b.keyword == "*INSTANCE") assembly_ = true;
        if (b.keyword == "*CLOAD") hasLoads_[0] = true;
        if (b.keyword == "*DLOAD") hasLoads_[1] = true;
        if (b.keyword == "*DSLOAD") hasLoads_[2] = true;
        if (b.keyword == "*TEMPERATURE" && b.stepIndex >= 0) hasLoads_[3] = true;
        if (b.keyword == "*EXPANSION") hasExpansion_ = true;
        if (b.keyword == "*DEPVAR") nSdv_ = 1;  // refined from results
    }
    if (deck_->steps().empty()) throw std::runtime_error("The input file has no *STEP");
    // Procedure line of every step, fixed-increment, user parameters kept (STABILIZE, ALPHA, APPLICATION, ...).
    for (const auto& st : deck_->steps()) {
        std::string line;
        for (int bi = st.blockIndex; bi <= st.endBlockIndex && line.empty(); ++bi) {
            const auto& b = deck_->blocks().at(static_cast<size_t>(bi));
            if (b.keyword != "*STATIC" && b.keyword != "*VISCO" && b.keyword != "*DYNAMIC") continue;
            line = b.keyword;
            if (b.keyword != "*VISCO") line += ", DIRECT";
            for (const auto& [k, v] : b.params) {
                if (k == "DIRECT" || k == "CETOL") continue;  // increments are fixed by ABSS (CETOL would make *VISCO adaptive)
                line += ", " + k + (v.empty() ? std::string() : "=" + v);
            }
            if (b.keyword == "*DYNAMIC") dynamic_ = true;
        }
        procLine_.push_back(line.empty() ? std::string("*STATIC, DIRECT") : line);
    }
    if (deck_->usesUserMaterial() || deck_->usesUserElement())
        if (cfg_.userSubroutine.empty())
            throw std::runtime_error("This model uses a user subroutine (UMAT/UEL). Provide the compiled object/library "
                                     "(or source with a working Abaqus compiler) in the run settings.");
    abq::LoadEvaluator check(*deck_);  // throws on unsupported load/BC features
    if (!cfg_.userSubroutine.empty()) {
        prof::Scope s("setup.user_subroutine");
        // Build (or accept a prebuilt) standardU.dll ONCE; every job then loads it without compiling.
        const auto lib = prepareUserSubroutine(cfg_.abaqusCommand, cfg_.userSubroutine,
                                               (fs::path(cfg_.scratchDir) / "usub_lib").string(),
                                               [this](const std::string& s) { log(s); });
        usubDir_ = lib.libraryDir;
        // Requirements of a foreign material library, checked against the deck before any job runs.
        auto upperStr = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        };
        bool userIc = false;
        for (const auto& b : deck_->blocks()) {
            if (b.keyword == "*USER MATERIAL" && lib.requiredNprops) {
                const int n = static_cast<int>(inp::parseNumber(b.param("CONSTANTS").value_or("0")).value_or(0));
                if (n != lib.requiredNprops)
                    throw std::runtime_error("*USER MATERIAL (line " + std::to_string(b.line) + ") has CONSTANTS=" + std::to_string(n) +
                                             ", but the material library requires exactly " + std::to_string(lib.requiredNprops) + ".");
            }
            if (b.keyword == "*DEPVAR" && lib.requiredNstatv && !b.data.empty()) {
                const auto f = inp::splitFields(b.data.front().text);
                const int n = f.empty() ? 0 : static_cast<int>(inp::parseNumber(f[0]).value_or(0));
                if (n != lib.requiredNstatv)
                    throw std::runtime_error("*DEPVAR (line " + std::to_string(b.line) + ") is " + std::to_string(n) +
                                             ", but the material library requires exactly " + std::to_string(lib.requiredNstatv) +
                                             " state variables.");
            }
            if (b.keyword == "*INITIAL CONDITIONS" && upperStr(b.param("TYPE").value_or("")) == "SOLUTION" && b.hasParam("USER"))
                userIc = true;
        }
        if (lib.requiredNtens == 6)
            for (const auto& e : deck_->mesh().elements)
                if (e.info.family != ElementFamily::Continuum3D) {
                    throw std::runtime_error("The material library requires 3-D stress states (NTENS=6), but element " +
                                             std::to_string(e.label) + " is " + e.type + ". Use 3-D continuum elements (C3D*).");
                }
        if (lib.requiresSdvini && !userIc)
            throw std::runtime_error("The material library initialises its own state variables, so the model needs "
                                     "'*INITIAL CONDITIONS, TYPE=SOLUTION, USER' (before the first *STEP); without it "
                                     "Abaqus would start from zero state, which this library does not support.");
    }
    jobs_ = std::make_unique<std::counting_semaphore<64>>(std::clamp(cfg_.maxConcurrentJobs, 1, 64));
    // Whole-model element set for *ELEMENT MATRIX OUTPUT. In an assembly it is defined inside *ASSEMBLY, one
    // instance-scoped block per instance (repeated *ELSET cards with the same name accumulate).
    const auto& mesh = deck_->mesh();
    std::map<std::int32_t, std::vector<std::int64_t>> byInstance;
    for (const auto& e : mesh.elements) byInstance[e.instance].push_back(e.label);
    for (const auto& [inst, labels] : byInstance) {
        elsetText_ += "*ELSET, ELSET=ABSS_ALL";
        if (inst >= 0) elsetText_ += ", INSTANCE=" + mesh.instanceNames.at(static_cast<size_t>(inst));
        elsetText_ += "\n";
        for (size_t i = 0; i < labels.size(); ++i)
            elsetText_ += std::to_string(labels[i]) + ((i + 1) % 16 == 0 || i + 1 == labels.size() ? "\n" : ", ");
    }
}

int AbaqusOracle::elsetLineOf(const inp::Deck& d) const {
    const auto& blocks = d.blocks();
    if (!assembly_) return blocks.at(static_cast<size_t>(d.steps().front().blockIndex)).line;
    int line = 0;  // decks given here are flattened (every *INCLUDE inlined), so all blocks are in the main file
    for (const auto& b : blocks)
        if (b.keyword == "*END ASSEMBLY" && b.fileIndex == 0) line = b.line;
    if (!line) throw std::runtime_error("The input file has *INSTANCE but no *END ASSEMBLY");
    return line;
}

std::string AbaqusOracle::nodeRef(std::int32_t n) const {
    const auto& k = deck_->mesh().nodeKeys.at(static_cast<size_t>(n));
    if (k.instance < 0) return std::to_string(k.label);
    return deck_->mesh().instanceNames.at(static_cast<size_t>(k.instance)) + "." + std::to_string(k.label);
}

std::int32_t AbaqusOracle::meshNode(std::int64_t number) const {
    auto it = filNode_.find(number);
    if (it == filNode_.end())
        throw std::runtime_error("Abaqus reported node " + std::to_string(number) + ", which ABSS cannot map to the model's mesh");
    return it->second;
}

void AbaqusOracle::buildNumbering(const abq::FilModel& fm) {
    const auto& mesh = deck_->mesh();
    const auto nn = static_cast<size_t>(mesh.nodeCount()), ne = static_cast<size_t>(mesh.elementCount());
    nodeNumber_.assign(nn, 0);
    elemNumber_.assign(ne, 0);
    if (!assembly_) {
        for (size_t n = 0; n < nn; ++n) nodeNumber_[n] = mesh.nodeKeys[n].label;
        for (size_t e = 0; e < ne; ++e) elemNumber_[e] = mesh.elements[e].label;
    } else {
        // Abaqus numbers instance by instance (deck order), labels ascending within each, then assembly-level entities.
        auto order = [&](size_t count, auto instOf, auto labelOf, std::vector<std::int64_t>& out) {
            std::vector<size_t> idx(count);
            for (size_t i = 0; i < count; ++i) idx[i] = i;
            auto rank = [&](std::int32_t inst) { return inst < 0 ? std::numeric_limits<std::int32_t>::max() : inst; };
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                return std::make_pair(rank(instOf(a)), labelOf(a)) < std::make_pair(rank(instOf(b)), labelOf(b));
            });
            for (size_t i = 0; i < count; ++i) out[idx[i]] = static_cast<std::int64_t>(i + 1);
        };
        order(nn, [&](size_t i) { return mesh.nodeKeys[i].instance; }, [&](size_t i) { return mesh.nodeKeys[i].label; }, nodeNumber_);
        order(ne, [&](size_t i) { return mesh.elements[i].instance; }, [&](size_t i) { return mesh.elements[i].label; }, elemNumber_);
    }
    filNode_.clear();
    for (size_t n = 0; n < nn; ++n) filNode_[nodeNumber_[n]] = static_cast<std::int32_t>(n);

    // Verify against Abaqus's own model definition: element types and connectivity must match exactly.
    std::string problem;
    if (!fm.elements.empty()) {
        if (fm.elements.size() != ne || fm.nodes.size() != nn)
            problem = "Abaqus has " + std::to_string(fm.nodes.size()) + " nodes and " + std::to_string(fm.elements.size()) +
                      " elements; ABSS read " + std::to_string(nn) + " and " + std::to_string(ne);
        for (size_t e = 0; e < ne && problem.empty(); ++e) {
            const auto& el = mesh.elements[e];
            auto it = fm.elements.find(elemNumber_[e]);
            bool ok = it != fm.elements.end() && it->second.second.size() >= el.conn.size();
            for (size_t c = 0; ok && c < el.conn.size(); ++c)
                ok = it->second.second[c] == nodeNumber_[static_cast<size_t>(el.conn[c])];
            if (!ok) problem = "element " + std::to_string(el.label) + " does not match Abaqus's element " + std::to_string(elemNumber_[e]);
        }
    } else {
        problem = "the nominal results file has no model definition records";
    }
    if (!problem.empty()) {
        if (assembly_)
            throw std::runtime_error("ABSS could not map Abaqus's node/element numbers to the parts and instances of this model (" +
                                     problem + "). Results would be attached to the wrong nodes, so the run is stopped.");
        log("Note: mesh check against Abaqus skipped (" + problem + ").");
        return;
    }
    double worst = 0, scale = std::max(1e-12, mesh.characteristicLength());
    for (size_t n = 0; n < nn; ++n) {
        const auto& x = fm.nodes.at(nodeNumber_[n]);
        for (size_t c = 0; c < x.size() && c < 3; ++c) worst = std::max(worst, std::abs(x[c] - mesh.xyz[3 * n + c]));
    }
    log("Mesh check against Abaqus: " + std::to_string(nn) + " nodes and " + std::to_string(ne) +
        " elements match (connectivity exact; largest coordinate difference " + std::to_string(worst) + ")");
    if (worst > 1e-6 * scale)
        log("Warning: node coordinates differ from Abaqus's by up to " + std::to_string(worst) +
            " (instance transform); results are mapped by numbering and connectivity, which match exactly.");
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
    prof::Scope s("job.cleanup");
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
    const fs::path root = inp::writeEditedDeck(*deck_, edits, dir.string(), name);
    // One flat file (every *INCLUDE expanded) so that steps and the assembly may live in included files.
    std::string flat;
    inlineIncludes(root, flat, 0);
    out.root = dir / (name + "_flat.inp");
    writeText(out.root, flat);
    out.deck = std::make_unique<inp::Deck>(inp::Deck::load(out.root.string()));
    return out;
}

std::string AbaqusOracle::modelDataOf(const inp::Deck& d, const fs::path& editedRoot) const {
    const auto lines = splitLines(readText(editedRoot));
    const int firstStepLine = d.blocks().at(static_cast<size_t>(d.steps().front().blockIndex)).line;
    const int elsetLine = elsetLineOf(d);
    std::string md;
    for (int i = 0; i < firstStepLine - 1 && i < static_cast<int>(lines.size()); ++i) {
        if (i + 1 == elsetLine) md += elsetText_;
        md += lines[static_cast<size_t>(i)] + "\n";
    }
    if (elsetLine >= firstStepLine) md += elsetText_;
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
    if (!usubDir_.empty()) {
        // Job-directory environment file: load the prebuilt user-subroutine library (no per-job compilation).
        const fs::path envFile = dir / "abaqus_v6.env";
        std::error_code ec;
        if (!fs::exists(envFile, ec)) {
            std::lock_guard<std::mutex> lk(logMutex_);
            if (!fs::exists(envFile, ec)) writeText(envFile, "usub_lib_dir = r\"" + usubDir_ + "\"\n");
        }
    }
    const std::string act = prof::activity().empty() ? std::string("unattributed") : prof::activity();
    const auto tw0 = std::chrono::steady_clock::now();
    jobs_->acquire();
    const auto tp0 = std::chrono::steady_clock::now();
    const auto wall0 = std::chrono::system_clock::now();
    prof::add("job.wait_slot", std::chrono::duration<double>(tp0 - tw0).count());
    detail::ProcessResult res;
    try {
        res = detail::runProcess(cfg_.abaqusCommand, args, dir.string(), 3600, cfg_.cancel);
    } catch (...) {
        jobs_->release();
        throw;
    }
    jobs_->release();
    ++jobCount_;
    {
        const double procWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count();
        prof::add("job.process", procWall);
        prof::add("jobs." + act, procWall);
        recordAbaqusPhases(res.output, wall0, procWall);
    }
    if (res.cancelled) throw std::runtime_error("Cancelled");
    if (res.timedOut) throw std::runtime_error("Abaqus job '" + job + "' exceeded the time limit and was stopped");
    const std::string dat = readText(dir / (job + ".dat")) + readText(dir / (job + ".msg"));
    if (res.exitCode != 0 || res.output.find("Abaqus JOB " + job + " COMPLETED") == std::string::npos ||
        dat.find("***ERROR") != std::string::npos) {
        std::string why = dat.substr(dat.find("***ERROR") == std::string::npos ? 0 : dat.find("***ERROR"), 600);
        if (why.empty()) why = res.output.substr(res.output.size() > 600 ? res.output.size() - 600 : 0);
        if (const std::string bl = usubDir_.empty() ? std::string() : readText(fs::path(usubDir_) / "abss_bridge.log"); !bl.empty())
            why += "\nMaterial helper log (abss_bridge.log): " + bl.substr(bl.size() > 1200 ? bl.size() - 1200 : 0);
        throw std::runtime_error("Abaqus job '" + job + "' did not complete. " + why);
    }
    if (dat.find("KEYWORD IS IGNORED IN A RESTART") != std::string::npos)
        throw std::runtime_error("Abaqus ignored a model keyword in restart job '" + job + "' (would silently change the model)");
    std::vector<abq::FilIncrement> incs;
    {
        prof::Scope s("job.parse_results");
        incs = abq::readFil((dir / (job + ".fil")).string());
    }
    if (incs.empty()) throw std::runtime_error("No results in " + job + ".fil");
    // Non-finite results: name the quantity (the branch adds the parameter); never difference NaNs silently.
    for (const auto& inc : incs) {
        for (const auto& [lab, v] : inc.U)
            for (double x : v)
                if (!std::isfinite(x)) throw std::runtime_error("Abaqus job '" + job + "' produced non-finite displacements (node " + std::to_string(lab) + ")");
        for (const auto& p : inc.points)
            for (const auto& [key, v] : p.values)
                for (size_t c = 0; c < v.size(); ++c)
                    if (!std::isfinite(v[c]))
                        throw std::runtime_error("Abaqus job '" + job + "' produced non-finite " + key + std::to_string(c + 1) +
                                                 " at element " + std::to_string(p.element) + ", point " + std::to_string(p.ip) +
                                                 ": the material returned NaN/Inf (a perturbed parameter probably left its admissible range)");
    }
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
    for (std::int32_t k = 0; k < dofs_.size(); ++k) {
        const auto& d = dofs_.dofs[static_cast<size_t>(k)];
        auto it = m.find(nodeNumber_[static_cast<size_t>(d.node)]);
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
            if (auto it = m.find(nodeNumber_[n]); it != m.end())
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
    prof::Scope total("phase.nominal");
    prof::Activity actNominal("nominal");
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
    before[elsetLineOf(*ed.deck)] += elsetText_;
    before[blocks.at(static_cast<size_t>(ed.deck->steps().front().blockIndex)).line] += "*FILE FORMAT, ASCII\n";
    // Measured on Abaqus 2024: *ELEMENT MATRIX OUTPUT in a model whose material has *EXPANSION makes Abaqus repeat the
    // first increment forever (step time never advances; removing *EXPANSION alone fixes it). Such models get their
    // element matrices from a separate job without thermal expansion and step temperatures: expansion does not change
    // the stiffness, and the matrices only precondition the solver (the tangent check measures them).
    const bool emoInNominal = !hasExpansion_;
    for (const auto& st : ed.deck->steps())
        before[blocks.at(static_cast<size_t>(st.endBlockIndex)).line] +=
            fieldRequests() +
            (emoInNominal ? "*ELEMENT MATRIX OUTPUT, ELSET=ABSS_ALL, STIFFNESS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=abss_emo, FREQUENCY=1\n" : "");
    if (dynamic_)  // element mass matrices (configuration independent): the first step's output is enough
        before[blocks.at(static_cast<size_t>(ed.deck->steps().front().endBlockIndex)).line] +=
            "*ELEMENT MATRIX OUTPUT, ELSET=ABSS_ALL, MASS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=abss_mass, FREQUENCY=1\n";
    // Derivatives are linearised at the nominal states, so those must be equilibrium states to well below the
    // derivative accuracy. Abaqus's default residual tolerance (R_n = 5e-3) leaves an equilibrium error that propagates
    // into every sensitivity of a nonlinear model (measured: 1e-4..1e-3). The nominal job therefore uses the same tight
    // controls as the validation reruns; if Abaqus cannot converge that tightly, the default tolerances are used and
    // the limitation is logged.
    auto writeNominal = [&](bool tight) {
        std::map<int, std::string> b2 = before;
        if (tight)
            for (const auto& st : ed.deck->steps())
                b2[blocks.at(static_cast<size_t>(st.endBlockIndex)).line] += kTightControls;
        std::string text;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (auto it = b2.find(static_cast<int>(i + 1)); it != b2.end()) text += it->second;
            text += lines[i] + "\n";
        }
        writeText(dir / "abss_nominal.inp", text);
    };
    writeNominal(true);
    log("Nominal Abaqus job: " + (dir / "abss_nominal.inp").string());
    if (progress) progress("Running nominal Abaqus job", 0.1);
    std::vector<abq::FilIncrement> incs;
    try {
        runJob(dir, "abss_nominal", "", false, 0, &incs);
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "Cancelled") throw;
        log(std::string("The nominal job did not converge with tight equilibrium tolerances (") + e.what() +
            "); repeating it with Abaqus's default tolerances. Sensitivities are then limited by the nominal equilibrium "
            "error (typically 1e-4..1e-3 relative for nonlinear increments).");
        writeNominal(false);
        runJob(dir, "abss_nominal", "", false, 0, &incs);
    }
    buildNumbering(abq::readFilModel((dir / "abss_nominal.fil").string()));
    if (emoInNominal) {
        prof::Scope pe("nominal.parse_element_matrices");
        emo_ = abq::readEmo((dir / "abss_emo.mtx").string());
    } else {
        // Element matrices from the same model without its step temperatures (see above).
        std::map<int, std::string> b3 = before, rep;
        b3[blocks.at(static_cast<size_t>(ed.deck->steps().front().endBlockIndex)).line] +=
            "*ELEMENT MATRIX OUTPUT, ELSET=ABSS_ALL, STIFFNESS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=abss_emo, FREQUENCY=1\n";
        for (const auto& b : blocks)
            if ((b.keyword == "*TEMPERATURE" && b.stepIndex >= 0) || b.keyword == "*EXPANSION") {
                for (int l = 0; l < b.keywordLineCount; ++l) rep[b.line + l] = "**";
                for (const auto& d : b.data) rep[d.lineNumber] = "**";
            }
        std::string text;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int ln = static_cast<int>(i + 1);
            if (auto it = b3.find(ln); it != b3.end()) text += it->second;
            if (auto it = rep.find(ln); it != rep.end()) text += it->second + "\n";
            else text += lines[i] + "\n";
        }
        const fs::path edir = fs::path(cfg_.scratchDir) / "matrices";
        fs::create_directories(edir);
        writeText(edir / "abss_matrices.inp", text);
        log("Thermal expansion: element matrices come from a separate job without *EXPANSION (Abaqus does not advance time "
            "when element matrix output meets thermal expansion); they precondition the solver and the tangent check measures them.");
        runJob(edir, "abss_matrices", "", false, 0);
        emo_ = abq::readEmo((edir / "abss_emo.mtx").string());
        emoAnyIncrement_ = true;
    }

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
        f.dynamic = procLine_.at(static_cast<size_t>(s0)).rfind("*DYNAMIC", 0) == 0;
        frames_.push_back(f);
    }
    if (dynamic_) {
        // Time-integration parameters exactly as Abaqus used them, from the nominal .dat (one block per step).
        const std::string dat = readText(dir / "abss_nominal.dat");
        static const std::regex stepHdr(R"(S T E P\s+(\d+))");
        std::vector<std::pair<size_t, int>> starts;
        for (std::sregex_iterator it(dat.begin(), dat.end(), stepHdr), end; it != end; ++it)
            starts.emplace_back(static_cast<size_t>(it->position()), std::stoi((*it)[1]));
        auto section = [&](int abqStep) {
            for (size_t k = 0; k < starts.size(); ++k)
                if (starts[k].second == abqStep)
                    return dat.substr(starts[k].first, k + 1 < starts.size() ? starts[k + 1].first - starts[k].first : std::string::npos);
            return std::string();
        };
        auto num = [](const std::string& s, const char* pat) -> std::optional<double> {
            std::smatch m;
            if (std::regex_search(s, m, std::regex(pat))) return std::stod(m[1]);
            return std::nullopt;
        };
        for (auto& f : frames_) {
            if (!f.dynamic) continue;
            const std::string s = section(f.abqStep);
            const auto a = num(s, R"(ALPHA\s+IS\s+([-+0-9.Ee]+))"), b = num(s, R"(BETA\s+IS\s+([-+0-9.Ee]+))"),
                       g = num(s, R"(GAMMA\s+IS\s+([-+0-9.Ee]+))");
            if (a && b && g) {
                f.alpha = *a, f.beta = *b, f.gamma = *g;
            } else if (s.find("BACKWARD EULER") != std::string::npos) {
                f.backwardEuler = true;
            } else {
                throw std::runtime_error("Could not identify the time-integration operator Abaqus used in dynamic step " +
                                         std::to_string(f.abqStep) + " (no Hilber-Hughes-Taylor or backward Euler "
                                         "parameters in the .dat); the inertia term cannot be reproduced.");
            }
        }
    }
    // DOF map: all DOFs present in the element matrices, plus constrained DOFs.
    const auto& mesh = deck_->mesh();
    std::set<std::pair<std::int32_t, int>> dofSet;
    const auto& firstEmo = emoFor(frames_[1].abqStep, frames_[1].abqInc);
    for (const auto& b : firstEmo)
        for (auto lab : b.nodes)
            for (int d : b.dofs) dofSet.insert({meshNode(lab), d});
    // Nodes without element matrices (reference nodes of couplings and rigid bodies) still carry DOFs: take every
    // component Abaqus reports in U. Then find the DOFs Abaqus itself eliminates (tie slaves, *EQUATION/*MPC dependents,
    // coupled and rigid-body nodes): prescribe every candidate with a distinct tiny value in one probe job; Abaqus keeps
    // its constraints and ignores the BC on a dependent DOF ("BCs on inactive dof"), so its U differs from the value
    // prescribed. Dependent DOFs are not unknowns: Abaqus computes them from the independent ones in every job.
    std::set<std::pair<std::int32_t, int>> candidates = dofSet;
    for (const auto& [lab, comps] : incs.front().U)
        if (auto it = filNode_.find(lab); it != filNode_.end())
            for (size_t c = 0; c < comps.size(); ++c) candidates.insert({it->second, static_cast<int>(c + 1)});
    std::set<std::pair<std::int32_t, int>> dependent;
    if (hasConstraints_ || candidates.size() != dofSet.size()) {
        if (progress) progress("Identifying constrained DOFs", 0.95);
        const fs::path cdir = fs::path(cfg_.scratchDir) / "dofcheck";
        fs::create_directories(cdir);
        const double L = std::max(1e-12, mesh.characteristicLength());
        std::map<std::pair<std::int32_t, int>, double> probe;
        std::ostringstream st;
        st << "*STEP, NAME=ABSS_DOFCHECK, NLGEOM=" << (frames_[1].nlgeom ? "YES" : "NO") << ", INC=1\n*STATIC, DIRECT\n"
           << inp::formatNumber(frames_[1].dt) << ", " << inp::formatNumber(frames_[1].dt) << "\n*BOUNDARY, OP=NEW\n";
        size_t k = 0;
        for (const auto& [n, d] : candidates) {
            const std::string vs = inp::formatNumber((d <= 3 ? 1e-7 * L : 1e-7) * (1.0 + static_cast<double>((k++ * 37) % 101) / 101.0));
            probe[{n, d}] = std::stod(vs);  // compare against exactly what Abaqus reads
            st << nodeRef(n) << ", " << d << ", " << d << ", " << vs << "\n";
        }
        st << "*NODE FILE, FREQUENCY=1\nU\n*END STEP\n";
        writeText(cdir / "abss_dofcheck.inp", modelDataOf(*ed.deck, ed.root) + st.str());
        prof::Activity actDof("nominal.dof_check");
        const auto chk = runJob(cdir, "abss_dofcheck", "", true, frames_[1].totalTime);
        for (const auto& [nd, v] : probe) {
            auto it = chk.U.find(nodeNumber_[static_cast<size_t>(nd.first)]);
            const double got = (it == chk.U.end() || static_cast<size_t>(nd.second) > it->second.size())
                                   ? std::numeric_limits<double>::quiet_NaN()
                                   : it->second[static_cast<size_t>(nd.second - 1)];
            if (!(std::abs(got - v) <= 1e-6 * std::abs(v))) dependent.insert(nd);
        }
        cleanup(cdir, "abss_dofcheck");
        for (const auto& nd : dependent) candidates.erase(nd);
        log("Constraint check: " + std::to_string(candidates.size()) + " independent DOFs; " + std::to_string(dependent.size()) +
            " DOFs are dependent or inactive (constraints, couplings, rigid bodies, unused components) and are computed "
            "by Abaqus in every job.");
        dofSet = candidates;
    }
    abq::LoadEvaluator ev(*deck_);
    std::vector<std::pair<std::int32_t, int>> cset;
    int inactive = 0;
    for (const auto& c : ev.prescribedDofs()) {
        if (dofSet.count(c)) cset.push_back(c);
        else ++inactive;  // e.g. PINNED/ENCASTRE DOF 3 on a 2D node, or a BC on a dependent DOF: Abaqus ignores it too
    }
    if (inactive) log(std::to_string(inactive) + " boundary-condition DOF(s) are not active on any element and are ignored (as Abaqus does).");
    dofs_ = DofMap{};
    dofIndex_.clear();
    for (const auto& [n, d] : dofSet) {
        dofIndex_[{n, d}] = dofs_.size();
        dofs_.dofs.push_back({n, static_cast<std::int8_t>(d)});
        dofs_.prescribed.push_back(std::binary_search(cset.begin(), cset.end(), std::make_pair(n, d)) ? 1 : 0);
    }
    rotNodes_.clear();
    for (std::int32_t n = 0; n < mesh.nodeCount(); ++n) {
        auto a = dofIndex_.find({n, 4}), b = dofIndex_.find({n, 5}), c = dofIndex_.find({n, 6});
        if (a != dofIndex_.end() && b != dofIndex_.end() && c != dofIndex_.end()) rotNodes_.push_back({a->second, b->second, c->second});
    }
    // Constrained set per step: BCs added later or released (OP=NEW) change which DOFs are unknowns.
    {
        const auto byStep = ev.constrainedByStep();
        prescribedByStep_.assign(byStep.size(), std::vector<std::uint8_t>(static_cast<size_t>(dofs_.size()), 0));
        releasedByStep_.assign(byStep.size(), {});
        for (size_t s = 0; s < byStep.size(); ++s)
            for (const auto& nd : byStep[s])
                if (auto it = dofIndex_.find(nd); it != dofIndex_.end()) prescribedByStep_[s][static_cast<size_t>(it->second)] = 1;
        size_t changes = 0;
        for (size_t s = 1; s < byStep.size(); ++s)
            for (std::int32_t k = 0; k < dofs_.size(); ++k) {
                if (prescribedByStep_[s - 1][static_cast<size_t>(k)] && !prescribedByStep_[s][static_cast<size_t>(k)])
                    releasedByStep_[s].push_back(k);
                if (prescribedByStep_[s - 1][static_cast<size_t>(k)] != prescribedByStep_[s][static_cast<size_t>(k)]) ++changes;
            }
        if (changes)
            log("Boundary conditions change between steps (" + std::to_string(changes) + " DOF changes): the unknowns are "
                "set per step, and a released DOF carries its reaction force, ramped down as Abaqus does.");
    }
    bool anyFree = false;
    for (size_t i = 1; i < frames_.size(); ++i) anyFree = anyFree || !freeIndicesAt(static_cast<int>(i)).empty();
    if (!anyFree) throw std::runtime_error("All DOFs are constrained; nothing to differentiate");
    if (dynamic_) {
        if (!dependent.empty())
            throw std::runtime_error("This dynamic model also has constraints (ties, couplings, MPCs, rigid bodies). With every "
                                     "independent DOF prescribed, Abaqus leaves inertia out of the reactions, and the inertia "
                                     "of constrained (dependent) DOFs reaches their masters only through the constraint "
                                     "equations, which Abaqus does not expose. This combination is not supported yet.");
        mass_ = assembleMass(abq::readEmo((dir / "abss_mass.mtx").string()));
        // Initial velocities (*INITIAL CONDITIONS, TYPE=VELOCITY: node or node set, DOF, value).
        v0_.assign(static_cast<size_t>(dofs_.size()), 0.0);
        for (const auto& b : deck_->blocks()) {
            if (b.keyword != "*INITIAL CONDITIONS") continue;
            std::string type = b.param("TYPE").value_or("");
            for (char& c : type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (type != "VELOCITY") continue;
            for (const auto& l : b.data) {
                const auto f = inp::splitFields(l.text);
                if (f.size() < 3) continue;
                const int dof = static_cast<int>(inp::parseNumber(f[1]).value_or(0));
                const double v = inp::parseNumber(f[2]).value_or(0.0);
                for (auto n : deck_->resolveNodes(f[0], b.instanceName))
                    if (auto it = dofIndex_.find({n, dof}); it != dofIndex_.end()) v0_[static_cast<size_t>(it->second)] = v;
            }
        }
        log("Dynamic steps: residual = -RF - M a(u). Abaqus leaves inertia out of reactions when every DOF is prescribed "
            "(measured), so ABSS adds it with Abaqus's own element mass matrices and the step's time integrator (HHT/Newmark "
            "parameters read from the nominal run), integrated on every branch's own history.");
    }
    if (hasContact_)
        log("Contact: contact forces are part of the residual (-RF). Derivatives are those of the current contact state; "
            "an opening/closing or stick/slip change inside a stencil is non-smooth and is flagged per increment.");

    // Self-check: re-evaluated BCs and concentrated loads must reproduce the nominal job's own output.
    double worstBc = 0, worstCf = 0;
    for (size_t i = 1; i < frames_.size(); ++i) {
        const auto st = ev.evaluate(frames_[i].step, frames_[i].stepTime, frames_[i].totalTime);
        const auto& inc = incs[i - 1];
        for (const auto& [nd, v] : st.bc) {
            if (!dofSet.count(nd)) continue;  // ignored by Abaqus (inactive or dependent DOF)
            auto it = inc.U.find(nodeNumber_[static_cast<size_t>(nd.first)]);
            if (it == inc.U.end() || static_cast<size_t>(nd.second) > it->second.size()) continue;
            worstBc = std::max(worstBc, std::abs(it->second[static_cast<size_t>(nd.second - 1)] - v) / (1e-12 + std::abs(v) + 1e-9));
        }
        // CF reported by Abaqus where ABSS expects no concentrated load is a mismatch too (two-directional check).
        // Follower loads (rotating) and released reactions (ramped by Abaqus) are excluded from the comparison.
        std::set<std::pair<std::int32_t, int>> skip = st.followerDofs;
        for (auto k : releasedByStep_.at(static_cast<size_t>(frames_[i].step)))
            skip.insert({dofs_.dofs[static_cast<size_t>(k)].node, dofs_.dofs[static_cast<size_t>(k)].dof});
        for (const auto& [lab, comps] : inc.CF)
            for (size_t c = 0; c < comps.size(); ++c)
                if (comps[c] != 0.0 && filNode_.count(lab) && !skip.count({filNode_.at(lab), static_cast<int>(c + 1)}) &&
                    !st.concentrated.count({filNode_.at(lab), static_cast<int>(c + 1)}))
                    worstCf = std::max(worstCf, 1.0);
        for (const auto& [nd, v] : st.concentrated) {
            auto it = inc.CF.find(nodeNumber_[static_cast<size_t>(nd.first)]);
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
    for (size_t ei = 0; ei < mesh.elements.size(); ++ei) {
        for (const auto& key : perElem[elemNumber_[ei]]) {
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
    const auto& emoBlocks = emoFor(f.abqStep, f.abqInc);
    // Dynamic increment: -dr/du = (1 + alpha) K + M / (beta dt^2) (HHT) or K + M / dt^2 (backward Euler).
    const double kScale = f.dynamic && !f.backwardEuler ? 1.0 + f.alpha : 1.0;
    const double mScale = !f.dynamic ? 0.0 : f.backwardEuler ? 1.0 / (f.dt * f.dt) : 1.0 / (f.beta * f.dt * f.dt);
    std::vector<Triplet> t;
    if (mScale != 0.0)
        for (std::int32_t r = 0; r < mass_.rows; ++r)
            for (std::int64_t q = mass_.rowPtr[static_cast<size_t>(r)]; q < mass_.rowPtr[static_cast<size_t>(r) + 1]; ++q)
                t.push_back({r, mass_.colIdx[static_cast<size_t>(q)], mScale * mass_.values[static_cast<size_t>(q)]});
    for (const auto& b : emoBlocks) {
        // Dependent DOFs (constraints) are not unknowns: their rows/columns are dropped, which makes this matrix an
        // approximation of the condensed tangent; the engine's tangent check then switches to matrix-free GMRES.
        std::vector<std::int32_t> idx;
        for (auto lab : b.nodes)
            for (int d : b.dofs) {
                auto it = dofIndex_.find({meshNode(lab), d});
                idx.push_back(it == dofIndex_.end() ? -1 : it->second);
            }
        const size_t n = idx.size();
        for (size_t r = 0; r < n; ++r)
            for (size_t c = 0; c < n; ++c)
                // exact zeros are kept as structural entries: the true tangent may be nonzero there (plasticity),
                // and the column grouping of the engine's assembled tangent relies on a complete pattern
                if (idx[r] >= 0 && idx[c] >= 0) t.push_back({idx[r], idx[c], kScale * b.K[r * n + c]});
    }
    // DOFs without element stiffness (reference nodes of couplings/rigid bodies; masters whose stiffness arrives only
    // through constraints) get the mean diagonal so the matrix stays a usable preconditioner. The tangent check then
    // sees the difference and the engine solves with the true (condensed) Abaqus operator by matrix-free GMRES.
    std::vector<double> diag(static_cast<size_t>(dofs_.size()), 0.0);
    for (const auto& e : t)
        if (e.row == e.col) diag[static_cast<size_t>(e.row)] += std::abs(e.value);
    double mean = 0;
    int nz = 0;
    for (double d : diag)
        if (d > 0) mean += d, ++nz;
    mean = nz ? mean / nz : 1.0;
    bool filled = false;
    for (size_t k = 0; k < diag.size(); ++k)
        if (diag[k] == 0.0) t.push_back({static_cast<std::int32_t>(k), static_cast<std::int32_t>(k), mean}), filled = true;
    // The pattern covers every true coupling only when all residual couplings come from element matrices.
    patternComplete_ = !filled && !hasConstraints_ && !hasContact_ && !deck_->usesUserElement();
    return CsrMatrix::fromTriplets(dofs_.size(), dofs_.size(), std::move(t));
}

CsrMatrix AbaqusOracle::assembleMass(const std::map<std::pair<int, int>, std::vector<abq::EmoBlock>>& m) const {
    if (m.empty()) throw std::runtime_error("Abaqus wrote no element mass matrices (needed for the inertia of dynamic steps)");
    std::vector<Triplet> t;
    for (const auto& b : m.begin()->second) {
        std::vector<std::int32_t> idx;
        for (auto lab : b.nodes)
            for (int d : b.dofs) {
                auto it = dofIndex_.find({meshNode(lab), d});
                idx.push_back(it == dofIndex_.end() ? -1 : it->second);
            }
        const size_t n = idx.size();
        for (size_t r = 0; r < n; ++r)
            for (size_t c = 0; c < n; ++c)
                if (idx[r] >= 0 && idx[c] >= 0 && b.K[r * n + c] != 0.0) t.push_back({idx[r], idx[c], b.K[r * n + c]});
    }
    return CsrMatrix::fromTriplets(dofs_.size(), dofs_.size(), std::move(t));
}

void AbaqusOracle::kinematics(int frame, const std::vector<double>& u, const std::vector<double>& un,
                              const std::vector<double>& vn, const std::vector<double>& an, std::vector<double>& a,
                              std::vector<double>* v) const {
    const auto& f = frames_.at(static_cast<size_t>(frame));
    const double dt = f.dt;
    a.assign(u.size(), 0.0);
    if (v) v->assign(u.size(), 0.0);
    for (size_t k = 0; k < u.size(); ++k) {
        if (f.backwardEuler) {
            const double vk = (u[k] - un[k]) / dt;
            a[k] = (vk - vn[k]) / dt;
            if (v) (*v)[k] = vk;
        } else {  // Newmark with the HHT parameters Abaqus used
            a[k] = (u[k] - un[k] - dt * vn[k] - dt * dt * (0.5 - f.beta) * an[k]) / (f.beta * dt * dt);
            if (v) (*v)[k] = vn[k] + dt * ((1.0 - f.gamma) * an[k] + f.gamma * a[k]);
        }
    }
}

std::unique_ptr<HistoryBranch> AbaqusOracle::openBranch(const std::vector<double>& delta) {
    auto b = std::make_unique<AbaqusBranch>(*this, delta, branchCounter_++);
    if (cfg_.batchBranches) {
        std::lock_guard<std::mutex> lk(groupMutex_);
        if (!groups_.empty()) throw std::logic_error("Replicated branches: a branch was opened after the groups were formed");
        openBranches_.push_back(b.get());
    }
    return b;
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
        // the procedure block of this step: same procedure and user parameters, fixed increments
        for (int bi = st.blockIndex; bi <= st.endBlockIndex; ++bi) {
            const auto& b = blocks.at(static_cast<size_t>(bi));
            if (b.keyword != "*STATIC" && b.keyword != "*VISCO" && b.keyword != "*DYNAMIC") continue;
            replace[b.line] = procLine_.at(s);
            for (int c = 1; c < b.keywordLineCount; ++c) replace[b.line + c] = "**";
            const std::string dt = inp::formatNumber(dts.front());
            if (!b.data.empty()) replace[b.data.front().lineNumber] = dt + ", " + inp::formatNumber(st.timePeriod);
            else before[b.line + b.keywordLineCount] += dt + ", " + inp::formatNumber(st.timePeriod) + "\n";
        }
        // Reference reruns: tighten the residual tolerance (Curie/Gauss: rerun noise ~ tol/h^k dominates validation).
        before[blocks.at(static_cast<size_t>(st.endBlockIndex)).line] +=
            fieldRequests() + kTightControls;
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
    o_.forgetBranch(this);
    if (!o_.cfg_.keepJobFiles) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
}

std::string AbaqusBranch::stepText(int frame, const std::vector<double>& u, bool commit) const {
    const auto& f = o_.frames_.at(static_cast<size_t>(frame));
    const auto st = loads_->evaluate(f.step, f.stepTime, f.totalTime);
    std::ostringstream os;
    os << "*STEP, NAME=ABSS_INC_" << frame << ", NLGEOM=" << (f.nlgeom ? "YES" : "NO") << ", INC=1\n";
    os << o_.procLine_.at(static_cast<size_t>(f.step)) << "\n" << inp::formatNumber(f.dt) << ", " << inp::formatNumber(f.dt) << "\n";
    if (!commit && frame > 1) os << "*RESTART, WRITE, FREQUENCY=0\n";
    os << "*BOUNDARY, OP=NEW\n";
    const auto& dm = o_.dofs_;
    const auto values = bcValues(frame, u);
    for (std::int32_t k = 0; k < dm.size(); ++k) {
        const auto& d = dm.dofs[static_cast<size_t>(k)];
        os << o_.nodeRef(d.node) << ", " << int(d.dof) << ", " << int(d.dof) << ", "
           << inp::formatNumber(values[static_cast<size_t>(k)]) << "\n";
    }
    // Reactions of BCs released at the start of this step act as loads that Abaqus ramps down over the step (RAMP)
    // or removes at once (STEP). Their size is this branch's own reaction at the end of the previous step.
    std::vector<std::string> releaseLines;
    if (!o_.releasedByStep_.at(static_cast<size_t>(f.step)).empty() && loads_->stepRamps(f.step)) {
        const double T = loads_->stepPeriod(f.step), factor = T > 0 ? 1.0 - f.stepTime / T : 0.0;
        for (auto k : o_.releasedByStep_[static_cast<size_t>(f.step)])
            if (auto it = releaseForce_.find(k); it != releaseForce_.end() && factor != 0.0) {
                const auto& d = dm.dofs[static_cast<size_t>(k)];
                releaseLines.push_back(o_.nodeRef(d.node) + ", " + std::to_string(int(d.dof)) + ", " + inp::formatNumber(it->second * factor));
            }
    }
    os << loadText(st, releaseLines);
    if (commit) os << o_.fieldRequests();
    else os << "*NODE FILE, FREQUENCY=1\nU, RF\n";
    // ABSS reads only the .fil file; without these Abaqus writes its default ODB output in every branch job.
    if (o_.cfg_.suppressOdbOutput) os << "*OUTPUT, FIELD, FREQUENCY=0\n*OUTPUT, HISTORY, FREQUENCY=0\n";
    os << "*END STEP\n";
    return os.str();
}

std::vector<double> AbaqusBranch::bcValues(int frame, const std::vector<double>& u) const {
    const auto& f = o_.frames_.at(static_cast<size_t>(frame));
    const auto st = loads_->evaluate(f.step, f.stepTime, f.totalTime);
    const auto& dm = o_.dofs_;
    std::vector<double> v(static_cast<size_t>(dm.size()));
    for (std::int32_t k = 0; k < dm.size(); ++k) {
        const auto& d = dm.dofs[static_cast<size_t>(k)];
        v[static_cast<size_t>(k)] = u.at(static_cast<size_t>(k));
        if (o_.prescribedAt(frame, k)) {
            auto it = st.bc.find({d.node, d.dof});
            v[static_cast<size_t>(k)] = it == st.bc.end() ? 0.0 : it->second;
        }
    }
    // Finite rotations in a restart step (NLGEOM): Abaqus composes R = exp(b - b_prev) R_prev. Free rotations are
    // targets for the total rotation vector, so write b = b_prev + log(R(phi) R(phi_prev)^T). Rotations held by the
    // user's BCs keep the BC history (b = g), exactly what the nominal analysis applied. In a job that starts at t = 0
    // (frame 1), b_prev = 0 and R_prev = I, so b = phi.
    if (f.nlgeom && frame > 1 && !lastCommitB_.empty())
        for (const auto& r : o_.rotNodes_) {
            bool held = false;
            for (int c = 0; c < 3; ++c) held = held || o_.prescribedAt(frame, r[static_cast<size_t>(c)]);
            std::array<double, 3> phi{}, prev{};
            for (int c = 0; c < 3; ++c) {
                phi[static_cast<size_t>(c)] = v[static_cast<size_t>(r[static_cast<size_t>(c)])];
                prev[static_cast<size_t>(c)] = lastCommitU_[static_cast<size_t>(r[static_cast<size_t>(c)])];
            }
            const auto d = held ? std::array<double, 3>{phi[0] - prev[0], phi[1] - prev[1], phi[2] - prev[2]}
                                : rotLog(rotMul(rotExp(phi), rotTranspose(rotExp(prev))));
            for (int c = 0; c < 3; ++c) {
                const auto k = static_cast<size_t>(r[static_cast<size_t>(c)]);
                if (held && o_.prescribedAt(frame, r[static_cast<size_t>(c)])) continue;  // user BC history as written
                v[k] = lastCommitB_[k] + d[static_cast<size_t>(c)];
            }
        }
    return v;
}

std::string AbaqusBranch::loadText(const abq::LoadState& st, const std::vector<std::string>& extraCload) const {
    std::ostringstream os;
    const auto& dm = o_.dofs_;
    const char* kws[] = {"*CLOAD", "*DLOAD", "*DSLOAD", "*TEMPERATURE"};
    for (int k = 0; k < 4; ++k) {
        if (!o_.hasLoads_[k] && !(k == 0 && !extraCload.empty())) continue;
        bool first = true;
        for (bool follower : {false, true}) {
            std::ostringstream block;
            for (const auto& l : st.loads)
                if (l.keyword == kws[k] && l.follower == follower) {
                    for (size_t c = 0; c < l.fields.size(); ++c) block << (c ? ", " : "") << l.fields[c];
                    block << "\n";
                }
            if (k == 0 && !follower)
                for (const auto& r : extraCload) block << r << "\n";
            if (block.str().empty() && follower) continue;  // the OP=NEW block is always written (clears old loads)
            // The first block replaces every load of this type (OP=NEW); a follower block after it adds (OP=MOD).
            os << kws[k] << ", OP=" << (first ? "NEW" : "MOD") << (follower ? ", FOLLOWER" : "") << "\n";
            if (block.str().empty() && k == 0) block << o_.nodeRef(dm.dofs[0].node) << ", " << int(dm.dofs[0].dof) << ", 0.\n";
            os << block.str();
            first = false;
        }
    }
    return os.str();
}

void AbaqusBranch::ensureDynamicState(int frame) {
    const auto& fr = o_.frames_;
    const auto& f = fr.at(static_cast<size_t>(frame));
    if (!f.dynamic || dynReadyFor_ == frame) return;
    const bool phaseStart = frame == 1 || !fr.at(static_cast<size_t>(frame) - 1).dynamic;
    if (!phaseStart) throw std::logic_error("Abaqus branch: dynamic state missing for increment " + std::to_string(frame));
    // Initial acceleration of this branch at the start of the dynamic step: M a0 = P(0) - I(u0) on the free DOFs.
    // P(0) - I(u0) = -RF of a static evaluation with every DOF held at the start state and the dynamic step's loads at
    // its time 0. The same job writes this branch's element mass matrices (density/geometry parameters change them).
    const size_t n = static_cast<size_t>(o_.dofs_.size());
    std::vector<double> u0 = frame == 1 ? std::vector<double>(n, 0.0) : lastCommitU_;
    const double t0 = fr.at(static_cast<size_t>(frame) - 1).totalTime;
    const auto st = loads_->evaluate(f.step, 0.0, t0);
    const std::string job = "b" + std::to_string(id_) + "_a0_" + std::to_string(frame);
    const double tiny = 1e-10;
    std::ostringstream os;
    if (frame == 1) os << modelData_;
    else os << "*HEADING\nABSS initial acceleration\n*RESTART, READ, STEP=" << (frame - 1) << ", INC=1, END STEP\n";
    // The step's own dynamic procedure (Abaqus forms and writes element mass matrices only for dynamic procedures);
    // every DOF is held, so RF carries no inertia, and over a vanishing increment with STEP loading RF = I(u0) - P(0).
    os << "*STEP, NAME=ABSS_A0, NLGEOM=" << (f.nlgeom ? "YES" : "NO") << ", INC=1, AMPLITUDE=STEP\n"
       << o_.procLine_.at(static_cast<size_t>(f.step)) << "\n"
       << inp::formatNumber(tiny) << ", " << inp::formatNumber(tiny) << "\n*BOUNDARY, OP=NEW\n";
    // Hold the start state: re-writing the last commit's BC values keeps every DOF (finite rotations included) in place.
    const auto& hold = frame == 1 || lastCommitB_.empty() ? u0 : lastCommitB_;
    for (std::int32_t k = 0; k < o_.dofs_.size(); ++k) {
        const auto& d = o_.dofs_.dofs[static_cast<size_t>(k)];
        os << o_.nodeRef(d.node) << ", " << int(d.dof) << ", " << int(d.dof) << ", " << inp::formatNumber(hold[static_cast<size_t>(k)]) << "\n";
    }
    os << loadText(st, {});
    if (!massLoaded_)
        os << "*ELEMENT MATRIX OUTPUT, ELSET=ABSS_ALL, MASS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=" << job << "m, FREQUENCY=1\n";
    os << "*NODE FILE, FREQUENCY=1\nU, RF\n*END STEP\n";
    writeText(dir_ / (job + ".inp"), os.str());
    prof::Activity actA0("dynamic.initial_acceleration");
    const auto inc = o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, true, t0 + tiny);
    if (!massLoaded_) {
        mass_ = o_.assembleMass(abq::readEmo((dir_ / (job + "m.mtx")).string()));
        massLoaded_ = true;
    }
    const auto rf = o_.dofVector(inc.RF);
    o_.cleanup(dir_, job);
    const auto F = o_.freeIndicesAt(frame);
    std::vector<double> a0(n, 0.0);
    if (!F.empty()) {
        SolveReport rep;
        auto fact = factorize(mass_.submatrix(F, F), SolverKind::Auto, &rep);
        if (!fact->ok()) throw std::runtime_error("The mass matrix could not be factorized (" + fact->message() + "); every free DOF needs mass in a dynamic step.");
        std::vector<double> b(F.size());
        for (size_t q = 0; q < F.size(); ++q) b[q] = -rf[static_cast<size_t>(F[q])];
        fact->solve(b, 1);
        for (size_t q = 0; q < F.size(); ++q) a0[static_cast<size_t>(F[q])] = b[q];
    }
    dynU_ = u0;
    dynV_ = frame == 1 ? o_.v0_ : std::vector<double>(n, 0.0);
    dynA_ = a0;
    dynReadyFor_ = frame;
}

std::vector<double> AbaqusBranch::inertia(int frame, const std::vector<double>& u) const {
    std::vector<double> a;
    o_.kinematics(frame, u, dynU_, dynV_, dynA_, a, nullptr);
    return mass_.multiply(a);
}

abq::FilIncrement AbaqusBranch::runIncrement(int frame, const std::vector<double>& u, bool commit, const std::string& job,
                                             const std::vector<double>* uNext, abq::FilIncrement* nextInc) {
    {
        prof::Scope gen("job.input_generation");
        std::string text;
        if (frame == 1) {
            text = modelData_;
            if (commit) text += "*RESTART, WRITE, FREQUENCY=1\n";
        } else {
            text = "*HEADING\nABSS history-branch increment\n*RESTART, READ, STEP=" + std::to_string(frame - 1) + ", INC=1, END STEP\n";
        }
        std::string st = stepText(frame, u, commit);
        // A commit step always writes restart data explicitly: a fused job's probe step turned restart writing off,
        // and whether that setting carries across the restart is not relied upon.
        if (commit && frame > 1) st.insert(st.find("*BOUNDARY, OP=NEW"), "*RESTART, WRITE, FREQUENCY=1\n");
        if (uNext) st += stepText(frame + 1, *uNext, false);
        text += st;
        writeText(dir_ / (job + ".inp"), text);
    }
    try {
        const double tEnd = o_.frames_.at(static_cast<size_t>(frame)).totalTime;
        if (!uNext) return o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, true, tEnd);
        std::vector<abq::FilIncrement> incs;
        o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, false, 0.0, &incs);
        // Exactly one increment in each of the two new steps, at the nominal times (no cutback, no subdivision).
        const int probeStep = incs.back().step;
        std::vector<const abq::FilIncrement*> c, p;
        for (const auto& x : incs) {
            if (x.step == probeStep) p.push_back(&x);
            else if (x.step == probeStep - 1) c.push_back(&x);
        }
        const double tNext = o_.frames_.at(static_cast<size_t>(frame) + 1).totalTime;
        auto near = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); };
        if (c.size() != 1 || p.size() != 1 || !near(c[0]->totalTime, tEnd) || !near(p[0]->totalTime, tNext))
            throw std::runtime_error("Oracle increments in job '" + job + "' were subdivided or mistimed (commit " +
                                     std::to_string(c.size()) + ", probe " + std::to_string(p.size()) +
                                     " increments); refusing to use them.");
        *nextInc = *p[0];
        return *c[0];
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "Cancelled") throw;
        // Name the perturbation this branch carries, so a failure points at the parameter that caused it.
        std::string which;
        for (size_t p = 0; p < delta_.size() && p < o_.cfg_.parameters.size(); ++p)
            if (delta_[p] != 0.0)
                which += (which.empty() ? "" : ", ") + o_.cfg_.parameters[p].name + " = " +
                         inp::formatNumber(o_.cfg_.parameters[p].nominal + delta_[p]);
        throw std::runtime_error(std::string(e.what()) + (which.empty() ? " [nominal branch]" : " [perturbed branch: " + which + "]"));
    }
}

std::vector<std::vector<double>> AbaqusBranch::residuals(int frame, const std::vector<std::vector<double>>& us) {
    if (frame != committed_ + 1) throw std::logic_error("Abaqus branch: probe out of sequence");
    if (auto* g = o_.group(this)) return g->probe(frame, this, us);
    ensureDynamicState(frame);
    const bool dyn = o_.frames_.at(static_cast<size_t>(frame)).dynamic;
    std::vector<std::vector<double>> out(us.size());
    std::vector<std::future<void>> fut;
    const std::string act = prof::activity();  // worker threads do not inherit the caller's activity
    for (size_t k = 0; k < us.size(); ++k)
        fut.push_back(std::async(std::launch::async, [&, k, act] {
            prof::Activity a(act);
            const std::string job = "b" + std::to_string(id_) + "_p" + std::to_string(frame) + "_" + std::to_string(probeCounter_++);
            const auto inc = runIncrement(frame, us[k], false, job);
            auto rf = o_.dofVector(inc.RF);
            for (double& x : rf) x = -x;  // r = F_ext - F_int = -RF on every held DOF
            if (dyn) {  // Abaqus leaves inertia out of reactions of prescribed DOFs: add -M a(u) (u as Abaqus applied it)
                const auto ma = inertia(frame, o_.dofVector(inc.U));
                for (size_t q = 0; q < rf.size(); ++q) rf[q] -= ma[q];
            }
            out[k] = std::move(rf);
            o_.cleanup(dir_, job);
        }));
    for (auto& f : fut) f.get();
    return out;
}

bool AbaqusBranch::canFuseNextProbe(int frame) const {
    const auto& fr = o_.frames_;
    if (!o_.cfg_.fuseCommitProbe || frame != committed_ + 1 || static_cast<size_t>(frame) + 1 >= fr.size()) return false;
    const auto& a = fr[static_cast<size_t>(frame)];
    const auto& b = fr[static_cast<size_t>(frame) + 1];
    // The probe step must not depend on this commit's results: kinematic state (dynamics), reactions of BCs that the
    // next step releases, or the finite-rotation composition of NLGEOM rotations.
    if (a.dynamic || b.dynamic) return false;
    if (b.step != a.step && !o_.releasedByStep_.at(static_cast<size_t>(b.step)).empty()) return false;
    if (b.nlgeom && !o_.rotNodes_.empty()) return false;
    return true;
}

FrameFields AbaqusBranch::commitAndProbe(int frame, const std::vector<double>& u,
                                         const std::vector<std::vector<double>>& uNext,
                                         std::vector<std::vector<double>>& nextResiduals) {
    if (!canFuseNextProbe(frame) || uNext.empty())
        throw std::logic_error("Abaqus branch: commit and probe cannot share a job here");
    if (o_.group(this)) throw std::logic_error("Replicated branches commit together (Oracle::runWave), not one by one");
    // Job 0 is the commit (its restart data continue the branch). Each further probe needs its own job: a sibling
    // replays the same commit step (writing no restart data) and then probes. Siblings run concurrently with job 0.
    const std::string job = "b" + std::to_string(id_) + "_c" + std::to_string(frame);
    auto written = bcValues(frame, u);
    nextResiduals.assign(uNext.size(), {});
    std::vector<abq::FilIncrement> siblingCommit(uNext.size());
    std::vector<std::future<void>> fut;
    const std::string act = prof::activity();
    for (size_t k = 1; k < uNext.size(); ++k)
        fut.push_back(std::async(std::launch::async, [&, k, act] {
            prof::Activity a(act + "_sibling");
            const std::string sj = job + "_s" + std::to_string(k);
            abq::FilIncrement next;
            siblingCommit[k] = runIncrement(frame, u, false, sj, &uNext[k], &next);
            nextResiduals[k] = o_.dofVector(next.RF);
            for (double& x : nextResiduals[k]) x = -x;
            o_.cleanup(dir_, sj);
        }));
    abq::FilIncrement next;
    std::exception_ptr err;
    abq::FilIncrement inc;
    try {
        inc = runIncrement(frame, u, true, job, &uNext[0], &next);
    } catch (...) {
        err = std::current_exception();
    }
    for (auto& f : fut) {
        try {
            f.get();
        } catch (...) {
            if (!err) err = std::current_exception();
        }
    }
    if (err) std::rethrow_exception(err);
    nextResiduals[0] = o_.dofVector(next.RF);
    for (double& x : nextResiduals[0]) x = -x;  // r = F_ext - F_int = -RF on every held DOF
    // The siblings' replayed commits must reproduce job 0's committed state (deterministic Abaqus): checked, not assumed.
    for (size_t k = 1; k < uNext.size(); ++k) {
        double diff = 0, scale = 1e-300;
        for (const auto* m : {&inc.U, &inc.RF})
            for (const auto& [lab, v] : *m) {
                const auto& other = (m == &inc.U ? siblingCommit[k].U : siblingCommit[k].RF);
                auto it = other.find(lab);
                if (it == other.end()) {
                    diff = INFINITY;
                    continue;
                }
                for (size_t c = 0; c < v.size() && c < it->second.size(); ++c) {
                    diff = std::max(diff, std::abs(v[c] - it->second[c]));
                    scale = std::max(scale, std::abs(v[c]));
                }
            }
        if (!(diff <= 1e-10 * scale))
            throw std::runtime_error("A replicated commit job did not reproduce the committed state (difference " +
                                     std::to_string(diff / scale) + " relative); Abaqus results are not deterministic "
                                     "here, so fused probes are unsafe. Rerun with fused probes disabled.");
    }
    return finishCommit(frame, inc, std::move(written), job, nullptr);
}

FrameFields AbaqusBranch::commit(int frame, const std::vector<double>& u, std::vector<double>* committedU) {
    if (frame != committed_ + 1) throw std::logic_error("Abaqus branch: commit out of sequence");
    if (o_.group(this)) throw std::logic_error("Replicated branches commit together (Oracle::runWave), not one by one");
    ensureDynamicState(frame);
    const std::string job = "b" + std::to_string(id_) + "_c" + std::to_string(frame);
    auto written = bcValues(frame, u);  // what this commit job writes (rotation bookkeeping for the next restart)
    const auto inc = runIncrement(frame, u, true, job);
    return finishCommit(frame, inc, std::move(written), job, committedU);
}

FrameFields AbaqusBranch::finishCommit(int frame, const abq::FilIncrement& inc, std::vector<double> written,
                                       const std::string& job, std::vector<double>* committedU) {
    lastCommitB_ = std::move(written);
    lastResidual_ = o_.dofVector(inc.RF);
    for (double& x : lastResidual_) x = -x;
    const auto uc = o_.dofVector(inc.U);
    const auto& fr = o_.frames_;
    if (fr.at(static_cast<size_t>(frame)).dynamic) {
        const auto ma = inertia(frame, uc);
        for (size_t q = 0; q < lastResidual_.size(); ++q) lastResidual_[q] -= ma[q];
        // Advance this branch's kinematic state with the committed displacements.
        std::vector<double> a, v;
        o_.kinematics(frame, uc, dynU_, dynV_, dynA_, a, &v);
        dynU_ = uc, dynV_ = std::move(v), dynA_ = std::move(a);
        dynReadyFor_ = static_cast<size_t>(frame) + 1 < fr.size() && fr[static_cast<size_t>(frame) + 1].dynamic ? frame + 1 : 0;
    }
    lastCommitU_ = uc;
    if (committedU) *committedU = uc;
    // Earlier commit jobs are still referenced by Abaqus restart chains; they are removed with the branch directory.
    lastCommitJob_ = job;
    committed_ = frame;
    auto f = o_.fieldsFrom(inc);
    const auto& dm = o_.dofs_;
    // Last increment of a step: remember the reactions of BCs that the next step releases.
    if (static_cast<size_t>(frame) + 1 < o_.frames_.size() && o_.frames_[static_cast<size_t>(frame) + 1].step != o_.frames_[static_cast<size_t>(frame)].step) {
        const auto rf = o_.dofVector(inc.RF);
        for (auto k : o_.releasedByStep_.at(static_cast<size_t>(o_.frames_[static_cast<size_t>(frame) + 1].step)))
            releaseForce_[k] = rf[static_cast<size_t>(k)];
    }
    // A commit prescribes every DOF, so Abaqus reports the (O(h^2), nonlinear) predictor residual as "RF" at the free
    // DOFs too. Reactions exist only at constrained DOFs (an equilibrium solve reports RF = 0 elsewhere): zero them.
    for (std::int32_t k = 0; k < dm.size(); ++k)
        if (!o_.prescribedAt(frame, k)) {
            const auto& d = dm.dofs[static_cast<size_t>(k)];
            const size_t i = static_cast<size_t>(d.node) * static_cast<size_t>(o_.nU_) + static_cast<size_t>(d.dof - 1);
            if (d.dof <= o_.nU_ && i < f.values[1].size()) f.values[1][i] = 0.0;
        }
    return f;
}

// ---- replicated branches ----------------------------------------------------------------------------------------
AbaqusOracle::~AbaqusOracle() = default;

void AbaqusOracle::forgetBranch(const AbaqusBranch* b) {
    std::lock_guard<std::mutex> lk(groupMutex_);
    openBranches_.erase(std::remove(openBranches_.begin(), openBranches_.end(), b), openBranches_.end());
    // The groups share restart chains with their members: keep them until the last grouped branch is gone.
    if (auto it = groupOf_.find(b); it != groupOf_.end()) {
        it->second->closed = true;  // a member is gone: the group may not run another job
        groupOf_.erase(it);
        if (groupOf_.empty()) groups_.clear();
    }
}

bool AbaqusOracle::formGroups() {
    if (!cfg_.batchBranches || batchRefused_) return false;
    std::lock_guard<std::mutex> lk(groupMutex_);
    if (!groups_.empty()) return true;
    if (batchRefused_) return false;
    std::string why;
    if (assembly_) why = "part/instance decks are not supported.";
    else if (dynamic_) why = "dynamic steps are not supported.";
    else if (deck_->usesUserElement()) why = "user elements are not supported.";
    else if (hasConstraints_ || hasContact_) why = "constraints and contact are not supported.";
    else if (openBranches_.empty()) why = "no branches.";
    if (why.empty()) {
        try {
            // One group per job slot. Group 0 holds the replay branch (opened first) and its replicas - alone when
            // there are other groups, so that replay-only probes (tangent columns, GMRES products) carry no idle
            // copies; perturbed branches are dealt round-robin over the other groups.
            const size_t g = std::clamp<size_t>(static_cast<size_t>(cfg_.maxConcurrentJobs), 1, openBranches_.size());
            std::vector<std::vector<AbaqusBranch*>> members(g);
            members[0].push_back(openBranches_[0]);
            for (size_t k = 1; k < openBranches_.size(); ++k)
                members[g == 1 ? 0 : 1 + (k - 1) % (g - 1)].push_back(openBranches_[k]);
            for (size_t q = 0; q < g; ++q) {
                if (members[q].empty()) continue;
                groups_.push_back(std::make_unique<ReplicaGroup>(*this, members[q], static_cast<int>(q)));
                for (auto* b : members[q]) groupOf_[b] = groups_.back().get();
            }
        } catch (const std::exception& e) {
            groups_.clear();
            groupOf_.clear();
            why = e.what();
        }
    }
    if (!why.empty()) {
        batchRefused_ = true;
        log("Replicated branches not used: " + why + " Every branch runs as its own Abaqus job.");
        return false;
    }
    log("Replicated branches: " + std::to_string(openBranches_.size()) + " history branches as disjoint model copies in " +
        std::to_string(groups_.size()) + " Abaqus job(s) per wave.");
    if (!usubDir_.empty())
        log("Replicated branches with a user subroutine: model copies carry shifted element numbers (NOEL) and suffixed "
            "material names (CMNAME). A subroutine whose result depends on those must be run without --batch.");
    return true;
}

ReplicaGroup* AbaqusOracle::group(const AbaqusBranch* b) {
    if (!formGroups()) return nullptr;
    std::lock_guard<std::mutex> lk(groupMutex_);
    auto it = groupOf_.find(b);
    if (it == groupOf_.end()) throw std::logic_error("Replicated branches: branch is not in any group");
    return it->second;
}

void AbaqusOracle::runWave(int frame, std::vector<WaveRequest>& wave, bool concurrent) {
    if (!formGroups()) return Oracle::runWave(frame, wave, concurrent);
    std::map<ReplicaGroup*, std::vector<WaveRequest*>> parts;
    for (auto& w : wave) {
        auto* b = dynamic_cast<AbaqusBranch*>(w.branch);
        if (!b) throw std::logic_error("Replicated branches: foreign branch in a wave");
        parts[group(b)].push_back(&w);
    }
    std::vector<std::future<void>> fut;
    for (auto& [g, ws] : parts) fut.push_back(std::async(std::launch::async, [&, g = g, act = prof::activity()] {
        prof::Activity a(act);
        g->runWave(frame, parts.at(g));
    }));
    std::exception_ptr err;
    for (auto& f : fut) {
        try {
            f.get();
        } catch (...) {
            if (!err) err = std::current_exception();
        }
    }
    if (err) std::rethrow_exception(err);
}

ReplicaGroup::ReplicaGroup(AbaqusOracle& o, const std::vector<AbaqusBranch*>& members, int index) : o_(o) {
    std::int64_t maxN = 0, maxE = 0;
    for (const auto& k : o_.deck_->mesh().nodeKeys) maxN = std::max<std::int64_t>(maxN, k.label);
    for (const auto& e : o_.deck_->mesh().elements) maxE = std::max<std::int64_t>(maxE, e.label);
    num_ = abq::replicaNumbering(maxN, maxE);
    for (auto* b : members) {
        bool isBase = true;
        for (double x : b->delta_) isBase = isBase && x == 0.0;
        const int replicas = isBase ? AbaqusOracle::kReplayReplicas : 1;
        for (int r = 0; r < replicas; ++r) {
            copiesOf_[b].push_back(static_cast<int>(copies_.size()));
            copies_.push_back({b, r});
        }
    }
    const auto n = static_cast<std::int64_t>(copies_.size());
    if (n * num_.nodeOffset > std::numeric_limits<std::int32_t>::max() || n * num_.elemOffset > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("the model is too large to hold " + std::to_string(n) + " copies within Abaqus's label range.");
    for (size_t c = 0; c < copies_.size(); ++c) modelData_ += abq::replicateModelData(copies_[c].b->modelData_, static_cast<int>(c), num_);
    zeros_.assign(static_cast<size_t>(o_.dofs_.size()), 0.0);
    // Dry run of the step data of every step (first increment of each), so unsupported loading is refused here and
    // the run falls back to one job per branch instead of failing later.
    for (size_t f = 1; f < o_.frames_.size(); ++f)
        if (f == 1 || o_.frames_[f].step != o_.frames_[f - 1].step)
            (void)abq::replicateStepData(copies_[0].b->stepText(static_cast<int>(f), zeros_, true), 1, num_);
    dir_ = fs::path(o_.cfg_.scratchDir) / ("replicas_" + std::to_string(index));
    fs::create_directories(dir_);
}

const std::vector<double>& ReplicaGroup::hold(const AbaqusBranch* b) const {
    // Copies without a state of their own in a probe job stay at their last committed state (or the initial state).
    return b->committed_ == 0 || b->lastCommitU_.empty() ? zeros_ : b->lastCommitU_;
}

std::string ReplicaGroup::header(int frame, bool commitWritesRestart) const {
    if (frame == 1) return modelData_ + (commitWritesRestart ? "*RESTART, WRITE, FREQUENCY=1\n" : "");
    return "*HEADING\nABSS replicated history branches\n*RESTART, READ, STEP=" + std::to_string(frame - 1) + ", INC=1, END STEP\n";
}

std::string ReplicaGroup::step(int frame, const std::vector<const std::vector<double>*>& copyU, bool commit) const {
    std::vector<std::string> per(copies_.size());
    for (size_t c = 0; c < copies_.size(); ++c)
        per[c] = abq::replicateStepData(copies_[c].b->stepText(frame, *copyU[c], commit), static_cast<int>(c), num_);
    std::string s = abq::mergeCopySteps(per);
    if (commit && frame > 1) s.insert(s.find("*BOUNDARY, OP=NEW"), "*RESTART, WRITE, FREQUENCY=1\n");
    return s;
}

void ReplicaGroup::checkReplicas(const abq::FilIncrement& inc, const AbaqusBranch* b, const std::string& job) const {
    const auto& cs = copiesOf_.at(b);
    if (cs.size() < 2) return;
    const auto ref = abq::extractCopy(inc, cs[0], num_);
    for (size_t r = 1; r < cs.size(); ++r) {
        const auto other = abq::extractCopy(inc, cs[r], num_);
        double diff = 0, scale = 1e-300;
        for (const auto* m : {&ref.U, &ref.RF})
            for (const auto& [lab, v] : *m) {
                const auto& om = m == &ref.U ? other.U : other.RF;
                auto it = om.find(lab);
                if (it == om.end()) {
                    diff = INFINITY;
                    continue;
                }
                for (size_t c = 0; c < v.size() && c < it->second.size(); ++c) {
                    diff = std::max(diff, std::abs(v[c] - it->second[c]));
                    scale = std::max(scale, std::abs(v[c]));
                }
            }
        if (!(diff <= 1e-10 * scale))
            throw std::runtime_error("Replicated job '" + job + "': replicas of one branch committed different states (" +
                                     std::to_string(diff / scale) + " relative). Rerun without --batch.");
    }
}

std::map<const AbaqusBranch*, std::vector<std::vector<double>>> ReplicaGroup::probeJobs(int frame, const Request& req) {
    if (frame != committed_ + 1) throw std::logic_error("Replicated branches: probe out of sequence");
    if (closed) throw std::logic_error("Replicated branches: a branch of this group was closed");
    std::map<const AbaqusBranch*, std::vector<std::vector<double>>> out;
    size_t jobs = 0;
    for (const auto& [b, us] : req) {
        const size_t r = copiesOf_.at(b).size();
        jobs = std::max(jobs, (us->size() + r - 1) / r);
        out[b].resize(us->size());
    }
    const double tEnd = o_.frames_.at(static_cast<size_t>(frame)).totalTime;
    const std::string act = prof::activity();
    // Result slots resolved before the job threads start (no map lookups while they write).
    std::vector<std::vector<std::vector<double>>*> slot(copies_.size(), nullptr);
    for (size_t c = 0; c < copies_.size(); ++c)
        if (auto it = out.find(copies_[c].b); it != out.end()) slot[c] = &it->second;
    auto one = [&, act](size_t j) {
        prof::Activity a(act);
        std::vector<const std::vector<double>*> cu(copies_.size());
        std::vector<std::pair<size_t, size_t>> used;  // (copy, probe index)
        for (size_t c = 0; c < copies_.size(); ++c) {
            const auto* b = copies_[c].b;
            const auto it = req.find(b);
            const size_t idx = j * copiesOf_.at(b).size() + static_cast<size_t>(copies_[c].replica);
            if (it != req.end() && idx < it->second->size()) {
                cu[c] = &(*it->second)[idx];
                used.emplace_back(c, idx);
            } else {
                cu[c] = &hold(b);
            }
        }
        const std::string job = "g_p" + std::to_string(frame) + "_" + std::to_string(counter_++);
        {
            prof::Scope gen("job.input_generation");
            writeText(dir_ / (job + ".inp"), header(frame, false) + step(frame, cu, false));
        }
        const auto inc = o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, true, tEnd);
        for (const auto& [c, idx] : used) {
            auto rf = o_.dofVector(abq::extractCopy(inc, static_cast<int>(c), num_).RF);
            for (double& x : rf) x = -x;  // r = F_ext - F_int = -RF on every held DOF
            (*slot[c])[idx] = std::move(rf);
        }
        o_.cleanup(dir_, job);
    };
    std::vector<std::future<void>> fut;
    for (size_t j = 0; j < jobs; ++j) fut.push_back(std::async(std::launch::async, one, j));
    std::exception_ptr err;
    for (auto& f : fut) {
        try {
            f.get();
        } catch (...) {
            if (!err) err = std::current_exception();
        }
    }
    if (err) std::rethrow_exception(err);
    return out;
}

std::vector<std::vector<double>> ReplicaGroup::probe(int frame, AbaqusBranch* b, const std::vector<std::vector<double>>& us) {
    prof::Activity act("batch." + (prof::activity().empty() ? std::string("probe") : prof::activity()));
    auto out = probeJobs(frame, Request{{b, &us}});
    return std::move(out[b]);
}

void ReplicaGroup::runWave(int frame, const std::vector<WaveRequest*>& wave) {
    if (wave.empty()) return;
    if (closed) throw std::logic_error("Replicated branches: a branch of this group was closed");
    const bool commit = wave[0]->commit;
    std::map<const AbaqusBranch*, WaveRequest*> byBranch;
    for (auto* w : wave) {
        auto* b = dynamic_cast<AbaqusBranch*>(w->branch);
        if (!b || !contains(b) || w->commit != commit || !byBranch.emplace(b, w).second)
            throw std::logic_error("Replicated branches: malformed wave");
    }
    if (!commit) {
        Request req;
        for (auto& [b, w] : byBranch)
            if (!w->probes.empty()) req[b] = &w->probes;
        prof::Activity act(wave.size() == 1 ? "batch." + wave[0]->activity : std::string("batch.probe_wave"));
        auto out = probeJobs(frame, req);
        for (auto& [b, w] : byBranch)
            if (!w->probes.empty()) w->residuals = std::move(out[b]);
        return;
    }
    // Commit wave: every copy commits (one restart chain); the next increment's probes ride along when all can.
    if (frame != committed_ + 1) throw std::logic_error("Replicated branches: commit out of sequence");
    if (byBranch.size() != copiesOf_.size()) throw std::logic_error("Replicated branches: a commit wave must commit every branch");
    bool fuse = true;
    for (auto& [b, w] : byBranch) fuse = fuse && !w->nextProbes.empty() && const_cast<AbaqusBranch*>(b)->canFuseNextProbe(frame);
    std::vector<const std::vector<double>*> cu(copies_.size()), nu(copies_.size());
    for (size_t c = 0; c < copies_.size(); ++c) {
        const auto* w = byBranch.at(copies_[c].b);
        cu[c] = &w->commitU;
        const auto r = static_cast<size_t>(copies_[c].replica);
        nu[c] = fuse && r < w->nextProbes.size() ? &w->nextProbes[r] : &w->commitU;
    }
    std::map<const AbaqusBranch*, std::vector<double>> written;
    for (auto& [b, w] : byBranch) written[b] = b->bcValues(frame, w->commitU);
    const std::string job = "g_c" + std::to_string(frame);
    prof::Activity act(fuse ? "batch.commit_fused" : "batch.commit");
    {
        prof::Scope gen("job.input_generation");
        std::string text = header(frame, true) + step(frame, cu, true);
        if (fuse) text += step(frame + 1, nu, false);
        writeText(dir_ / (job + ".inp"), text);
    }
    const double tEnd = o_.frames_.at(static_cast<size_t>(frame)).totalTime;
    abq::FilIncrement cInc, pInc;
    if (!fuse) {
        cInc = o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, true, tEnd);
    } else {
        std::vector<abq::FilIncrement> incs;
        o_.runJob(dir_, job, frame == 1 ? "" : lastCommitJob_, false, 0.0, &incs);
        const int probeStep = incs.back().step;
        std::vector<const abq::FilIncrement*> c, p;
        for (const auto& x : incs) {
            if (x.step == probeStep) p.push_back(&x);
            else if (x.step == probeStep - 1) c.push_back(&x);
        }
        const double tNext = o_.frames_.at(static_cast<size_t>(frame) + 1).totalTime;
        auto near = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); };
        if (c.size() != 1 || p.size() != 1 || !near(c[0]->totalTime, tEnd) || !near(p[0]->totalTime, tNext))
            throw std::runtime_error("Replicated job '" + job + "' increments were subdivided or mistimed; refusing to use them.");
        cInc = *c[0];
        pInc = *p[0];
    }
    for (auto& [b, w] : byBranch) {
        const auto& cs = copiesOf_.at(b);
        checkReplicas(cInc, b, job);
        w->fields = const_cast<AbaqusBranch*>(b)->finishCommit(frame, abq::extractCopy(cInc, cs[0], num_), std::move(written[b]), job, nullptr);
        w->nextResiduals.clear();
        if (fuse)
            for (size_t r = 0; r < w->nextProbes.size() && r < cs.size(); ++r) {
                auto rf = o_.dofVector(abq::extractCopy(pInc, cs[r], num_).RF);
                for (double& x : rf) x = -x;
                w->nextResiduals.push_back(std::move(rf));
            }
    }
    lastCommitJob_ = job;
    committed_ = frame;
}

}  // namespace

std::unique_ptr<Oracle> makeAbaqusOracle(const AbaqusRunConfig& cfg) { return std::make_unique<AbaqusOracle>(cfg); }

}  // namespace abss
