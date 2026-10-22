// abss-cli: headless driver for demo runs, INP inspection, Abaqus runs, and measured benchmarks.
#include "abss/core/profile.hpp"
#include "abss/core/project_io.hpp"
#include "abss/inp/deck.hpp"
#include "abss/linalg/parallel.hpp"
#include "abss/oracle/abaqus.hpp"
#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace abss;

namespace {

void usage() {
    std::cout <<
        R"(abss-cli - Abaqus Black-Box Sensitivity Studio (headless)

  abss-cli demo <plate|cantilever|truss|truss-plastic|quad> [--order K] [--params a,b,..] [--rel-step h]
                [--validate] [--out file.abss] [--threads-parallel]
      Run the SYNTHETIC demo model (not Abaqus) end to end and print validation evidence.
  abss-cli inspect <model.inp>
      Parse an INP (with includes) and list discovered parameters with source locations.
  abss-cli abaqus-info [--check-licence <scratchdir>]
      Detect Abaqus and optionally run a tiny licence test job.
  abss-cli run <model.inp> --params id1,id2 [--order K] [--rel-step h] [--jobs N] [--user file.f|.obj] [--scratch dir]
                [--out file.abss] [--validate] [--val-step-factor F] [--keep] [--no-tangent-check]
                [--profile run.json] [--no-overlap] [--no-fuse] [--odb-output]
      --no-overlap / --no-fuse / --odb-output restore the earlier job schedule (separate probe and commit waves,
      separate probe jobs, default ODB output in branch jobs) for before/after measurements; results are identical.
      --batch runs every history branch as a disjoint copy of the model in one Abaqus job per wave (flat decks with
      supported keywords; otherwise it falls back to one job per branch and says so).
      A parameter id may be a linked direction "combo:<id>*<w>+<id>*<w>" (one scalar moving several values), e.g.
      combo:umat:MAT:props:1*1+umat:MAT:props:2*-1 keeps PROPS[1]+PROPS[2] constant.
      Real Abaqus sensitivity run (requires Abaqus + licence). --val-step-factor scales the validation reruns' FD
      step relative to the engine step (default 1; larger steps reduce rerun noise for quantities near zero).
  abss-cli fafd <model.inp> --params id1,id2 [--order K] [--rel-step h] [--jobs N] [--user ...] [--scratch dir]
                [--out file.abss] [--profile run.json]
      BASELINE, not ABSS: full-analysis central finite differences (2 complete Abaqus analyses per parameter for
      K = 1) with the same steps, time grid, tolerances and outputs as ABSS validation. Output is labelled FAFD.
  abss-cli compare <reference.abss> <test.abss> [--fields U,RF,S] [--json out.json]
      Per derivative and field: max |reference|, max |error| and scaled error over every frame.
  abss-cli usersub <file.f|file.obj|standardU.dll|folder> [--out dir]
      Prepare a UMAT/UEL library once (build from source/.obj with `abaqus make`, or accept a prebuilt
      standardU.dll) and report the compiler toolchain. Prebuilt libraries need no compiler.
  abss-cli bench [--n N]
      Measured benchmarks: Kokkos vs serial kernels, factorization reuse across RHS.
  abss-cli show <file.abss>
      Summarize a saved results file (works without Abaqus).
)";
}

std::map<std::string, std::string> parseOpts(int argc, char** argv, int start) {
    std::map<std::string, std::string> o;
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            const std::string key = a.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) o[key] = argv[++i];
            else o[key] = "1";
        }
    }
    return o;
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, c))
        if (!t.empty()) out.push_back(t);
    return out;
}

void printQuality(const ResultStore& s) {
    for (const auto& [d, q] : s.quality) {
        std::cout << "  " << s.derivativeLabel("u", d) << ": " << qualityName(q.status);
        if (q.fdRelativeError >= 0) std::cout << ", rerun-FD max rel. error " << q.fdRelativeError;
        if (q.tangentCheckError >= 0) std::cout << ", tangent check " << q.tangentCheckError;
        std::cout << "\n";
        for (const auto& n : q.notes) std::cout << "      - " << n << "\n";
    }
}

int cmdDemo(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    const std::string which = argv[2];
    auto o = parseOpts(argc, argv, 3);
    synthetic::ModelDef def;
    if (which == "plate") def = synthetic::plateWithHole();
    else if (which == "cantilever") def = synthetic::cantileverBlock();
    else if (which == "truss") def = synthetic::twoBarTruss(false);
    else if (which == "truss-plastic") def = synthetic::twoBarTruss(true);
    else if (which == "quad") def = synthetic::singleQuad(true);
    else return usage(), 2;
    synthetic::SyntheticOracle oracle(def);
    ResultStore store;
    std::cout << "SYNTHETIC DEMO DATA (built-in test model, not Abaqus): " << def.title << "\n";
    auto u = oracle.runNominal(store, nullptr);
    SensitivityRequest req;
    req.maxOrder = o.count("order") ? std::stoi(o["order"]) : 1;
    if (o.count("params")) {
        for (const auto& n : split(o["params"], ','))
            for (size_t i = 0; i < store.parameters.size(); ++i)
                if (store.parameters[i].name == n || store.parameters[i].id == n) req.parameters.push_back(static_cast<int>(i));
    } else {
        for (size_t i = 0; i < store.parameters.size(); ++i) req.parameters.push_back(static_cast<int>(i));
    }
    if (o.count("rel-step"))
        for (int p : req.parameters)
            req.absoluteSteps.push_back(std::stod(o["rel-step"]) * std::abs(store.parameters[static_cast<size_t>(p)].nominal));
    req.parallelBranches = o.count("threads-parallel") > 0;
    std::cout << "Oracle evaluations planned: "
              << estimateOracleEvaluations(static_cast<int>(u.size()) - 1, static_cast<int>(req.parameters.size()),
                                           req.maxOrder, 0, req.tangentCheck)
              << "\n";
    RunControl ctl;
    ctl.log = [](const std::string& s) { std::cout << "  " << s << "\n"; };
    const auto rep = runSensitivities(oracle, u, req, store, ctl);
    if (o.count("validate")) {
        ValidationOptions vo;
        std::cout << validateAgainstReruns(oracle, req, store, vo, ctl);
    }
    std::cout << "Evidence:\n";
    printQuality(store);
    std::cout << "Engine time " << rep.seconds << " s, synthetic oracle evaluations " << oracle.evaluations() << "\n";
    if (o.count("out")) {
        ProjectMeta meta;
        meta.solverBackend = "synthetic demo";
        meta.computeBackend = rep.computeBackend;
        saveProject(o["out"], store, meta);
        std::cout << "Saved " << o["out"] << "\n";
    }
    return 0;
}

int cmdInspect(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    const auto deck = inp::Deck::load(argv[2]);
    std::cout << "Files: " << deck.files().size() << ", keyword blocks: " << deck.blocks().size()
              << ", nodes: " << deck.mesh().nodeCount() << ", elements: " << deck.mesh().elementCount()
              << ", steps: " << deck.steps().size() << "\n";
    for (const auto& d : deck.diagnostics()) std::cout << "  [diag] " << d.file << ":" << d.line << " " << d.message << "\n";
    for (const auto& m : deck.missingIncludes()) std::cout << "  [missing] " << m << "\n";
    const auto params = inp::discoverParameters(deck);
    std::cout << params.size() << " parameter candidates:\n";
    for (const auto& p : params) {
        std::cout << "  " << categoryName(p.category) << " | " << p.name << " = " << p.nominal
                  << (p.confidence == Confidence::OpaqueIndexed ? " (opaque index)" : "") << " | " << p.id;
        if (!p.targets.empty())
            std::cout << " | " << std::filesystem::path(p.targets[0].ref.file).filename().string() << ":" << p.targets[0].ref.line;
        if (!p.supported) std::cout << " | unsupported: " << p.note;
        std::cout << "\n";
    }
    return 0;
}

int cmdAbaqusInfo(int argc, char** argv) {
    auto o = parseOpts(argc, argv, 2);
    auto inst = detectAbaqus();
    std::cout << inst.message << "\n";
    std::cout << "User subroutines: " << detectCompilerToolchain().message << "\n";
    if (o.count("check-licence")) {
        checkAbaqusLicense(inst, o["check-licence"]);
        std::cout << inst.message << "\n";
    }
    for (const auto& [k, v] : abaqusCapabilities()) std::cout << "  " << k << ": " << v << "\n";
    return inst.found ? 0 : 1;
}

/// Benchmark record: run configuration, problem size, wall-clock phases, summed per-category profile, engine diagnostics.
void writeProfile(const std::string& path, const std::string& method, int argc, char** argv, const AbaqusRunConfig& cfg,
                  const ResultStore& store, const SensitivityRequest& req, const std::map<std::string, double>& elapsed,
                  const SensitivityReport* rep, const Oracle* oracle = nullptr, const nlohmann::json& extra = {}) {
    nlohmann::json j;
    j["method"] = method;
    std::string cmd;
    for (int i = 0; i < argc; ++i) cmd += (i ? " " : "") + std::string(argv[i]);
    j["command"] = cmd;
    j["inp"] = cfg.inpPath;
    j["inp_sha256"] = sha256File(cfg.inpPath);
    j["max_concurrent_jobs"] = cfg.maxConcurrentJobs;
    j["user_subroutine"] = cfg.userSubroutine;
    j["compute_backend"] = par::backendDescription();
    j["max_order"] = req.maxOrder;
    j["parallel_branches"] = req.parallelBranches;
    j["overlap_waves"] = req.overlapWaves;
    j["tangent_check"] = req.tangentCheck;
    j["fuse_commit_probe"] = cfg.fuseCommitProbe;
    j["suppress_odb_output"] = cfg.suppressOdbOutput;
    j["batch_branches"] = cfg.batchBranches;
    nlohmann::json ps = nlohmann::json::array();
    for (size_t k = 0; k < req.parameters.size(); ++k) {
        const auto& p = store.parameters[static_cast<size_t>(req.parameters[k])];
        ps.push_back({{"id", p.id}, {"nominal", p.nominal},
                      {"step", k < req.absoluteSteps.size() ? req.absoluteSteps[k] : -1.0}});
    }
    j["parameters"] = ps;
    j["nodes"] = store.mesh.nodeCount();
    j["elements"] = store.mesh.elementCount();
    j["material_points"] = store.points.pointCount();
    j["increments"] = static_cast<int>(store.frames.size()) - 1;
    if (oracle) {
        j["dofs"] = oracle->dofMap().size();
        j["free_dofs"] = static_cast<int>(oracle->dofMap().freeIndices().size());
    }
    j["elapsed_seconds"] = elapsed;
    j["profile"] = nlohmann::json::parse(prof::Registry::instance().toJson());
    if (rep) {
        j["oracle_evaluations"] = rep->oracleEvaluations;
        j["engine_seconds"] = rep->seconds;
        nlohmann::json fr = nlohmann::json::array();
        for (const auto& f : rep->frames)
            fr.push_back({{"frame", f.frame}, {"tangent_check", f.tangentCheckError}, {"solve_residual", f.solveResidual},
                          {"krylov", f.krylovUsed}, {"krylov_matvecs", f.krylovMatvecs},
                          {"krylov_residual", f.krylovResidual}, {"krylov_converged", f.krylovConverged},
                          {"probed_tangent", f.probedTangent}, {"tangent_colours", f.tangentColours},
                          {"assembled_tangent_check", f.assembledTangentCheck}, {"solver", f.solver},
                          {"commit_residual", f.commitResidual}, {"reprobe_passes", f.reprobePasses}});
        j["frames"] = fr;
    }
    nlohmann::json q = nlohmann::json::object();
    for (const auto& [d, r] : store.quality)
        q[store.derivativeLabel("u", d)] = {{"status", qualityName(r.status)}, {"fd_rel_error", r.fdRelativeError},
                                            {"tangent_check", r.tangentCheckError}, {"notes", r.notes}};
    j["quality"] = q;
    for (const auto& [k, v] : extra.items()) j[k] = v;
    std::ofstream(path) << j.dump(2) << "\n";
    std::cout << "Profile written to " << path << "\n";
}

/// Shared by `run` and `fafd`: deck, discovered + linked parameters, job limits. Returns 0 on success.
int setupAbaqusRun(char** argv, std::map<std::string, std::string>& o, AbaqusInstallation& inst, AbaqusRunConfig& cfg) {
    inst = detectAbaqus();
    if (!inst.found) {
        std::cerr << inst.message << "\n";
        return 1;
    }
    const auto deck = inp::Deck::load(argv[2]);
    cfg.inpPath = std::filesystem::absolute(argv[2]).string();
    cfg.abaqusCommand = inst.command;
    cfg.scratchDir = o.count("scratch") ? o["scratch"] : (std::filesystem::temp_directory_path() / "abss_run").string();
    cfg.parameters = inp::discoverParameters(deck);
    // Linked parameters: "combo:<id>*<w>+<id>*<w>..." is one scalar t (nominal 0) that moves every listed value by
    // w * t - the derivative along a constrained direction, e.g. two fractions whose sum must stay fixed.
    for (const auto& id : split(o.count("params") ? o["params"] : "", ',')) {
        if (id.rfind("combo:", 0) != 0) continue;
        ParameterInfo c;
        c.id = id;
        c.category = ParamCategory::Custom;
        c.confidence = Confidence::OpaqueIndexed;
        c.mode = EditMode::Offset;
        c.nominal = 0.0;
        std::string label;
        for (const auto& term : split(id.substr(6), '+')) {
            const auto star = term.rfind('*');
            const std::string pid = star == std::string::npos ? term : term.substr(0, star);
            const double w = star == std::string::npos ? 1.0 : std::stod(term.substr(star + 1));
            const auto it = std::find_if(cfg.parameters.begin(), cfg.parameters.end(), [&](const ParameterInfo& p) { return p.id == pid; });
            if (it == cfg.parameters.end()) {
                std::cerr << "Unknown parameter in " << id << ": " << pid << "\n";
                return 2;
            }
            for (auto t : it->targets) {
                t.weight = w;
                c.targets.push_back(t);
            }
            std::ostringstream os;
            os << (label.empty() ? "" : (w < 0 ? " - " : " + ")) << (label.empty() && w < 0 ? "-" : "")
               << (std::abs(w) == 1.0 ? std::string() : std::to_string(std::abs(w)) + "*") << it->name;
            label += os.str();
        }
        c.name = "linked: " + label;
        c.description = "One scalar t moving several values together (" + label + "), derivative along that direction";
        cfg.parameters.push_back(std::move(c));
    }
    cfg.maxConcurrentJobs = o.count("jobs") ? std::stoi(o["jobs"]) : 1;
    if (o.count("user")) cfg.userSubroutine = std::filesystem::absolute(o["user"]).string();
    cfg.keepJobFiles = o.count("keep") > 0;
    if (o.count("no-fuse")) cfg.fuseCommitProbe = false;
    if (o.count("odb-output")) cfg.suppressOdbOutput = false;
    if (o.count("batch")) cfg.batchBranches = true;
    cfg.log = [](const std::string& s) { std::cout << "  " << s << "\n"; };
    return 0;
}

/// Requested parameter indices (by id or name) and optional relative step. Returns 0 on success.
int selectParameters(std::map<std::string, std::string>& o, const ResultStore& store, SensitivityRequest& req) {
    req.maxOrder = o.count("order") ? std::stoi(o["order"]) : 1;
    for (const auto& id : split(o.count("params") ? o["params"] : "", ','))
        for (size_t i = 0; i < store.parameters.size(); ++i)
            if (store.parameters[i].id == id || store.parameters[i].name == id) req.parameters.push_back(static_cast<int>(i));
    if (req.parameters.empty()) {
        std::cerr << "No parameters selected (--params id1,id2). Use `abss-cli inspect` to list ids.\n";
        return 2;
    }
    if (o.count("rel-step"))
        for (int p : req.parameters)
            req.absoluteSteps.push_back(std::stod(o["rel-step"]) * std::abs(store.parameters[static_cast<size_t>(p)].nominal));
    return 0;
}

int cmdRun(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    auto o = parseOpts(argc, argv, 3);
    AbaqusInstallation inst;
    AbaqusRunConfig cfg;
    if (int rc = setupAbaqusRun(argv, o, inst, cfg)) return rc;
    std::atomic<bool> cancel{false};
    cfg.cancel = &cancel;
    using clk = std::chrono::steady_clock;
    const auto tRun0 = clk::now();
    auto since = [](clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); };
    std::map<std::string, double> elapsed;  // wall-clock phases (not summed over workers)
    auto t0 = clk::now();
    auto oracle = makeAbaqusOracle(cfg);
    elapsed["setup"] = since(t0);
    ResultStore store;
    t0 = clk::now();
    auto u = oracle->runNominal(store, nullptr);
    elapsed["nominal"] = since(t0);
    SensitivityRequest req;
    if (int rc = selectParameters(o, store, req)) return rc;
    req.parallelBranches = cfg.maxConcurrentJobs > 1;
    if (o.count("no-tangent-check")) req.tangentCheck = false;
    if (o.count("no-overlap")) req.overlapWaves = false;
    req.concurrency = cfg.maxConcurrentJobs;
    RunControl ctl;
    ctl.log = cfg.log;
    t0 = clk::now();
    const auto rep = runSensitivities(*oracle, u, req, store, ctl);
    elapsed["sensitivity"] = since(t0);
    std::cout << "Engine wall time " << rep.seconds << " s\n";
    if (o.count("validate")) {
        ValidationOptions vo;
        if (o.count("val-step-factor")) vo.stepFactor = std::stod(o.at("val-step-factor"));
        t0 = clk::now();
        std::cout << validateAgainstReruns(*oracle, req, store, vo, ctl);
        elapsed["validation"] = since(t0);
    }
    printQuality(store);
    if (o.count("out")) {
        ProjectMeta meta;
        meta.inpPath = cfg.inpPath;
        meta.inpSha256 = sha256File(cfg.inpPath);
        meta.solverBackend = inst.version;
        meta.computeBackend = par::backendDescription();
        t0 = clk::now();
        saveProject(o["out"], store, meta);
        elapsed["serialize"] = since(t0);
        std::cout << "Saved " << o["out"] << "\n";
    }
    elapsed["total"] = since(tRun0);
    if (o.count("profile")) writeProfile(o["profile"], "abss", argc, argv, cfg, store, req, elapsed, &rep, oracle.get());
    return 0;
}

/// FAFD baseline (NOT the ABSS method): central finite differences of complete, re-equilibrated Abaqus analyses at
/// a +- m h. Same deck, parameters, steps h, stencil, fixed nominal time grid, tight controls and outputs as the ABSS
/// validation reruns; the reruns are independent and run concurrently up to --jobs. Results are labelled FAFD.
int cmdFafd(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    auto o = parseOpts(argc, argv, 3);
    AbaqusInstallation inst;
    AbaqusRunConfig cfg;
    if (int rc = setupAbaqusRun(argv, o, inst, cfg)) return rc;
    std::atomic<bool> cancel{false};
    cfg.cancel = &cancel;
    using clk = std::chrono::steady_clock;
    const auto tRun0 = clk::now();
    auto since = [](clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); };
    std::map<std::string, double> elapsed;
    auto t0 = clk::now();
    auto oracle = makeAbaqusOracle(cfg);
    elapsed["setup"] = since(t0);
    ResultStore store;
    t0 = clk::now();
    {
        // ABSS machinery used here only to fix the time grid and the output layout (it also writes element matrices
        // and checks DOFs, which FAFD itself does not need): reported as grid setup, not part of the FAFD workflow.
        prof::Activity act("fafd.grid_setup");
        oracle->runNominal(store, nullptr);
    }
    elapsed["grid_setup"] = since(t0);
    // The FAFD workflow's own nominal analysis: one plain analysis on the same deck, grid and controls.
    std::vector<FrameFields> plainNominal;
    t0 = clk::now();
    {
        prof::Activity act("fafd.nominal");
        oracle->solvePerturbed(std::vector<double>(store.parameters.size(), 0.0), &plainNominal);
    }
    elapsed["nominal"] = since(t0);
    SensitivityRequest req;
    if (int rc = selectParameters(o, store, req)) return rc;
    const int K = req.maxOrder;
    const int M = defaultHalfWidth(K);
    std::vector<int> offsets;
    for (int m = -M; m <= M; ++m) offsets.push_back(m);
    const size_t nAll = store.parameters.size();
    std::vector<double> h(req.parameters.size());
    for (size_t j = 0; j < req.parameters.size(); ++j) {
        const auto& p = store.parameters[static_cast<size_t>(req.parameters[j])];
        h[j] = j < req.absoluteSteps.size() && req.absoluteSteps[j] > 0 ? req.absoluteSteps[j]
                                                                       : defaultRelativeStep(K) * (p.absoluteStep() / p.relativeStep);
    }
    // Analyses: (parameter, offset) for every offset with a non-zero weight in some order 1..K; offset 0 shared.
    std::vector<bool> offsetUsed(offsets.size(), false);
    for (int k = 1; k <= K; ++k) {
        const auto w = stencilWeights(offsets, k);
        for (size_t b = 0; b < offsets.size(); ++b)
            if (std::abs(w[b]) > 1e-14) offsetUsed[b] = true;
    }
    struct Run { size_t j; int m; std::vector<FrameFields> fields; bool ok = false; };
    std::vector<Run> runs;
    for (size_t b = 0; b < offsets.size(); ++b)  // the zero offset is the nominal analysis above (no second run)
        if (offsetUsed[b] && offsets[b] == 0)
            runs.push_back({0, 0, plainNominal, plainNominal.size() == store.frames.size()});
    for (size_t j = 0; j < req.parameters.size(); ++j)
        for (size_t b = 0; b < offsets.size(); ++b)
            if (offsetUsed[b] && offsets[b] != 0) runs.push_back({j, offsets[b], {}, false});
    std::cout << "FAFD: " << runs.size() << " full analyses (" << req.parameters.size() << " parameter(s), order " << K
              << ", " << cfg.maxConcurrentJobs << " concurrent job(s))\n";
    t0 = clk::now();
    {
        prof::Scope ws("stage.fafd_analyses");
        auto one = [&](size_t t) {
            if (runs[t].m == 0) return;  // the nominal analysis, already run
            prof::Activity act("fafd.analysis");
            std::vector<double> delta(nAll, 0.0);
            if (runs[t].m != 0) delta[static_cast<size_t>(req.parameters[runs[t].j])] = runs[t].m * h[runs[t].j];
            runs[t].ok = !oracle->solvePerturbed(delta, &runs[t].fields).empty() && runs[t].fields.size() == store.frames.size();
        };
        if (cfg.maxConcurrentJobs <= 1) {
            for (size_t t = 0; t < runs.size(); ++t) one(t);
        } else {
            std::vector<std::future<void>> fut;
            for (size_t t = 0; t < runs.size(); ++t) fut.push_back(std::async(std::launch::async, one, t));
            for (auto& f : fut) f.get();
        }
    }
    elapsed["sensitivity"] = since(t0);
    int failed = 0;
    for (const auto& r : runs) failed += r.ok ? 0 : 1;
    auto runOf = [&](size_t j, int m) -> const Run* {
        for (const auto& r : runs)
            if ((m == 0 && r.m == 0) || (m != 0 && r.j == j && r.m == m)) return &r;
        return nullptr;
    };
    for (size_t j = 0; j < req.parameters.size(); ++j) {
        const int pi = req.parameters[j];
        for (int k = 1; k <= K; ++k) {
            const auto w = stencilWeights(offsets, k);
            const auto spec = DerivativeSpec::pure(pi, k);
            auto& q = store.quality[spec];
            q.scheme = "FAFD: central differences of complete Abaqus analyses (fixed nominal grid, tight controls)";
            q.stepSize = h[j];
            bool ok = true;
            for (size_t b = 0; b < offsets.size(); ++b)
                if (std::abs(w[b]) > 1e-14) {
                    const Run* r = runOf(j, offsets[b]);
                    ok = ok && r && r->ok;
                }
            if (!ok) {
                q.status = QualityStatus::Failed;
                q.notes.push_back("FAFD: an analysis of the stencil failed or did not reproduce the nominal grid.");
                continue;
            }
            q.status = QualityStatus::Unverified;
            q.notes.push_back("FAFD baseline result (not computed by the ABSS engine).");
            const double hk = std::pow(h[j], k);
            for (size_t fr = 1; fr < store.frames.size(); ++fr)  // like ABSS: increments 1..n (no initial state)
                for (size_t f = 0; f < store.fields.size(); ++f) {
                    std::vector<double> d;
                    for (size_t b = 0; b < offsets.size(); ++b) {
                        if (std::abs(w[b]) <= 1e-14) continue;
                        const auto& v = runOf(j, offsets[b])->fields[fr].values[f];
                        if (d.empty()) d.assign(v.size(), 0.0);
                        for (size_t c = 0; c < v.size() && c < d.size(); ++c) d[c] += w[b] * v[c] / hk;
                    }
                    store.set(static_cast<int>(f), spec, static_cast<int>(fr), std::move(d));
                }
        }
    }
    store.backend = "FAFD (full-analysis finite differences) on " + inst.version;
    store.log.push_back("Sensitivities in this file were computed by FAFD, not by the ABSS engine.");
    std::cout << "FAFD sensitivity wall time " << elapsed["sensitivity"] << " s, failed analyses " << failed << "\n";
    if (o.count("out")) {
        ProjectMeta meta;
        meta.inpPath = cfg.inpPath;
        meta.inpSha256 = sha256File(cfg.inpPath);
        meta.solverBackend = store.backend;
        meta.computeBackend = par::backendDescription();
        t0 = clk::now();
        saveProject(o["out"], store, meta);
        elapsed["serialize"] = since(t0);
        std::cout << "Saved " << o["out"] << " (FAFD)\n";
    }
    elapsed["total"] = since(tRun0);
    if (o.count("profile")) {
        nlohmann::json ex;
        ex["fafd_analyses"] = std::count_if(runs.begin(), runs.end(), [](const Run& r) { return r.m != 0; });
        ex["fafd_nominal_analyses"] = 1;
        ex["fafd_failed"] = failed;
        writeProfile(o["profile"], "fafd", argc, argv, cfg, store, req, elapsed, nullptr, oracle.get(), ex);
    }
    return failed ? 3 : 0;
}

/// Field-by-field agreement of two results files over every frame, for every derivative they share (matched by
/// parameter id). Errors are absolute and scaled by the reference field's largest magnitude (robust near zero).
int cmdCompare(int argc, char** argv) {
    if (argc < 4) return usage(), 2;
    auto o = parseOpts(argc, argv, 4);
    const auto ref = loadProject(argv[2]);
    const auto tst = loadProject(argv[3]);
    nlohmann::json out = nlohmann::json::array();
    if (ref.frames.size() != tst.frames.size()) {
        std::cerr << "Different frame counts: " << ref.frames.size() << " vs " << tst.frames.size() << "\n";
        return 3;
    }
    const auto onlyFields = split(o.count("fields") ? o["fields"] : "", ',');
    std::printf("%-44s %-6s %12s %12s %12s\n", "derivative", "field", "max|ref|", "max|err|", "scaled err");
    for (const auto& dr : ref.derivatives()) {
        if (dr.order() == 0) continue;
        DerivativeSpec dt;
        bool found = true;
        for (int p : dr.params) {
            const auto& id = ref.parameters[static_cast<size_t>(p)].id;
            int q = -1;
            for (size_t i = 0; i < tst.parameters.size(); ++i)
                if (tst.parameters[i].id == id) q = static_cast<int>(i);
            if (q < 0) found = false;
            dt.params.push_back(q);
        }
        if (!found) continue;
        for (size_t f = 0; f < ref.fields.size(); ++f) {
            const auto& name = ref.fields[f].name;
            if (!onlyFields.empty() && std::find(onlyFields.begin(), onlyFields.end(), name) == onlyFields.end()) continue;
            const int g = tst.fieldIndex(name);
            if (g < 0 || !ref.has(static_cast<int>(f), dr) || !tst.has(g, dt)) continue;
            double refMax = 0, errMax = 0;
            long long n = 0, missing = 0;
            for (size_t fr = 1; fr < ref.frames.size(); ++fr) {  // increments 1..n; the initial state carries none
                const auto* a = ref.get(static_cast<int>(f), dr, static_cast<int>(fr));
                const auto* b = tst.get(g, dt, static_cast<int>(fr));
                if (!a && !b) continue;  // e.g. the initial state, which carries no derivative in either file
                if (!a || !b || a->size() != b->size()) {
                    ++missing;
                    continue;
                }
                for (size_t c = 0; c < a->size(); ++c) {
                    if (!std::isfinite((*a)[c]) || !std::isfinite((*b)[c])) continue;
                    refMax = std::max(refMax, std::abs((*a)[c]));
                    errMax = std::max(errMax, std::abs((*a)[c] - (*b)[c]));
                    ++n;
                }
            }
            const double scaled = refMax > 0 ? errMax / refMax : (errMax > 0 ? INFINITY : 0.0);
            const auto label = ref.derivativeLabel("", dr);
            std::printf("%-44s %-6s %12.4e %12.4e %12.4e%s\n", label.c_str(), name.c_str(), refMax, errMax, scaled,
                        missing ? "  (frames missing)" : "");
            nlohmann::json row;
            row["derivative"] = label;
            row["parameter_ids"] = nlohmann::json::array();
            for (int p : dr.params) row["parameter_ids"].push_back(ref.parameters[static_cast<size_t>(p)].id);
            row["field"] = name;
            row["ref_max_abs"] = refMax;
            row["max_abs_error"] = errMax;
            row["scaled_error"] = std::isfinite(scaled) ? nlohmann::json(scaled) : nlohmann::json(nullptr);
            row["values_compared"] = n;
            row["frames_missing"] = missing;
            out.push_back(row);
        }
    }
    if (o.count("json")) std::ofstream(o["json"]) << out.dump(2) << "\n";
    return 0;
}

int cmdUsersub(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    auto o = parseOpts(argc, argv, 3);
    auto inst = detectAbaqus();
    if (!inst.found) {
        std::cerr << inst.message << "\n";
        return 1;
    }
    const auto tc = detectCompilerToolchain();
    std::cout << "Toolchain: " << tc.message << "\n";
    const std::string out = o.count("out") ? o["out"] : (std::filesystem::temp_directory_path() / "abss_usersub").string();
    const auto lib = prepareUserSubroutine(inst.command, argv[2], out, [](const std::string& s) { std::cout << "  " << s << "\n"; });
    std::cout << "Library folder: " << lib.libraryDir << " (" << lib.kind << (lib.builtHere ? ", built now" : "") << ")\n";
    return 0;
}

int cmdShow(int argc, char** argv) {
    if (argc < 3) return usage(), 2;
    ProjectMeta meta;
    const auto s = loadProject(argv[2], &meta);
    std::cout << s.title << (s.synthetic ? "  [SYNTHETIC DEMO DATA]" : "") << "\nbackend: " << s.backend
              << "\nnodes " << s.mesh.nodeCount() << ", elements " << s.mesh.elementCount() << ", frames " << s.frames.size()
              << "\nINP: " << meta.inpPath << " sha256 " << meta.inpSha256 << "\n";
    printQuality(s);
    return 0;
}

template <class F>
double timeIt(F&& f, int reps) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / reps;
}

int cmdBench(int argc, char** argv) {
    auto o = parseOpts(argc, argv, 2);
    const int n = o.count("n") ? std::stoi(o["n"]) : 400;  // grid n x n
    par::initialize();
    std::cout << "Compute backend: " << par::backendDescription() << "\n";
    // 1) FD stencil over large field arrays (the per-increment output-derivative kernel)
    const size_t len = static_cast<size_t>(n) * n * 20;
    std::vector<std::vector<double>> arrays(5, std::vector<double>(len));
    for (size_t k = 0; k < arrays.size(); ++k)
        for (size_t i = 0; i < len; ++i) arrays[k][i] = std::sin(1e-3 * double(i) + double(k));
    std::vector<const std::vector<double>*> in;
    for (auto& a : arrays) in.push_back(&a);
    const std::vector<double> w = {1. / 12, -2. / 3, 0., 2. / 3, -1. / 12};
    std::vector<double> out;
    par::combine(in, w, out, false);  // warm-up
    const double tSerial = timeIt([&] { par::combine(in, w, out, true); }, 10);
    const double tPar = timeIt([&] { par::combine(in, w, out, false); }, 10);
    std::printf("stencil combine, 5 x %zu doubles: serial %.4f s, Kokkos %.4f s, speedup %.2fx\n", len, tSerial, tPar,
                tSerial / tPar);
    // 2) SpMV on a 2D 5-point operator
    std::vector<Triplet> t;
    auto id = [n](int i, int j) { return i * n + j; };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            t.push_back({id(i, j), id(i, j), 4.0});
            if (i > 0) t.push_back({id(i, j), id(i - 1, j), -1.0});
            if (i < n - 1) t.push_back({id(i, j), id(i + 1, j), -1.0});
            if (j > 0) t.push_back({id(i, j), id(i, j - 1), -1.0});
            if (j < n - 1) t.push_back({id(i, j), id(i, j + 1), -1.0});
        }
    const auto A = CsrMatrix::fromTriplets(n * n, n * n, t);
    std::vector<double> x(static_cast<size_t>(n) * n, 1.0), y;
    const int nrhs = 16;
    std::vector<double> X(x.size() * nrhs, 1.0), Y;
    const double sS = timeIt([&] { par::spmm(A, X, nrhs, Y, true); }, 5);
    const double sP = timeIt([&] { par::spmm(A, X, nrhs, Y, false); }, 5);
    std::printf("SpMM (%d x %d, nnz %lld, %d RHS): serial %.4f s, Kokkos %.4f s, speedup %.2fx\n", A.rows, A.cols,
                static_cast<long long>(A.nnz()), nrhs, sS, sP, sS / sP);
    // 3) Factorization reuse across parameter RHS vs refactorizing per RHS
    const int m = std::min(n, 150);
    std::vector<Triplet> t2;
    auto id2 = [m](int i, int j) { return i * m + j; };
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < m; ++j) {
            t2.push_back({id2(i, j), id2(i, j), 4.0});
            if (i > 0) t2.push_back({id2(i, j), id2(i - 1, j), -1.0});
            if (i < m - 1) t2.push_back({id2(i, j), id2(i + 1, j), -1.0});
            if (j > 0) t2.push_back({id2(i, j), id2(i, j - 1), -1.0});
            if (j < m - 1) t2.push_back({id2(i, j), id2(i, j + 1), -1.0});
        }
    const auto B = CsrMatrix::fromTriplets(m * m, m * m, t2);
    std::vector<std::vector<double>> rhs(12, std::vector<double>(static_cast<size_t>(m) * m, 1.0));
    for (auto kind : availableSolvers()) {
        if (kind == SolverKind::DenseLU && m * m > 3000) continue;
        if (kind == SolverKind::KokkosCG) continue;
        SolveReport rep;
        const double tReuse = timeIt([&] { solveMany(B, rhs, kind, &rep); }, 1);
        const double tNaive = timeIt([&] {
            for (const auto& r : rhs) solveMany(B, {r}, kind, nullptr);
        }, 1);
        std::printf("%-28s n=%d, 12 RHS: one factorization %.4f s vs refactor per RHS %.4f s (%.2fx), residual %.1e\n",
                    solverName(kind), m * m, tReuse, tNaive, tNaive / tReuse, rep.relativeResidual);
    }
    {
        std::vector<double> xs;
        const auto t0 = std::chrono::steady_clock::now();
        const int it = par::conjugateGradient(B, rhs[0], xs, 1e-10, 100000, false);
        const double tk = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        xs.assign(xs.size(), 0.0);  // restart from zero: same work as the Kokkos solve
        const auto t1 = std::chrono::steady_clock::now();
        par::conjugateGradient(B, rhs[0], xs, 1e-10, 100000, true);
        const double ts = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        std::printf("Jacobi-PCG n=%d: %d iterations, serial %.4f s, Kokkos %.4f s, speedup %.2fx\n", m * m, it, ts, tk, ts / tk);
    }
    // 4) Full synthetic engine run timing
    {
        synthetic::SyntheticOracle oracle(synthetic::plateWithHole(16, 20));
        ResultStore store;
        auto u = oracle.runNominal(store, nullptr);
        SensitivityRequest req;
        req.parameters = {0, 1, 2, 3, 4, 5, 6};
        req.maxOrder = 2;
        RunControl ctl;
        const auto rep = runSensitivities(oracle, u, req, store, ctl);
        std::printf("Synthetic plate (%d nodes, %d elements, %zu increments), 7 parameters, order 2: %.3f s, %lld oracle evals\n",
                    store.mesh.nodeCount(), store.mesh.elementCount(), store.frames.size() - 1, rep.seconds, oracle.evaluations());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(), 2;
    const std::string cmd = argv[1];
    try {
        if (cmd == "demo") return cmdDemo(argc, argv);
        if (cmd == "inspect") return cmdInspect(argc, argv);
        if (cmd == "abaqus-info") return cmdAbaqusInfo(argc, argv);
        if (cmd == "run") return cmdRun(argc, argv);
        if (cmd == "bench") return cmdBench(argc, argv);
        if (cmd == "show") return cmdShow(argc, argv);
        if (cmd == "usersub") return cmdUsersub(argc, argv);
        if (cmd == "fafd") return cmdFafd(argc, argv);
        if (cmd == "compare") return cmdCompare(argc, argv);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

