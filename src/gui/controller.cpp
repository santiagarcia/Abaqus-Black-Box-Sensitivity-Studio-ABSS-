#include "abss/gui/controller.hpp"

#include "abss/inp/deck.hpp"
#include "abss/linalg/parallel.hpp"
#include "abss/oracle/abaqus.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace abss::gui {

// ---- demo catalogue --------------------------------------------------------------------------------------------

const std::vector<DemoInfo>& demoModels() {
    static const std::vector<DemoInfo> v = {
        {DemoModel::PlateWithHole, "plate", "Plate with a hole (2D, plasticity)",
         "Quarter plate with a circular hole, plane strain, J2 plasticity with load, unload and reload. "
         "Parameters: stiffness, yield stress, hardening, edge loads and the hole radius (geometry).",
         {"E", "sigma_y", "R_hole"}},
        {DemoModel::CantileverBlock, "cantilever", "Cantilever block (3D hexahedra)",
         "Clamped 3D block with a downward tip force, elastic-plastic. Parameters: stiffness, Poisson's ratio, "
         "yield, hardening, tip force and beam length.",
         {"E", "F_tip"}},
        {DemoModel::TwoBarTrussElastic, "truss", "Two-bar truss (elastic, analytic answers)",
         "Two collinear bars with a force and a prescribed displacement. Every sensitivity has a closed-form answer.",
         {"E", "F", "g"}},
        {DemoModel::TwoBarTrussPlastic, "truss-plastic", "Two-bar truss (elastic-plastic, unloading)",
         "The same truss with yielding, hardening and unloading: a path-dependent history.",
         {"E", "sigma_y", "H"}},
        {DemoModel::SingleQuadPlastic, "quad", "Single element (uniform plastic state)",
         "One plane-strain element pulled by a prescribed displacement.",
         {"E", "nu", "sigma_y"}},
    };
    return v;
}

const DemoInfo& demoInfo(DemoModel m) {
    for (const auto& d : demoModels())
        if (d.id == m) return d;
    return demoModels().front();
}

std::optional<DemoModel> demoFromKey(const QString& key) {
    for (const auto& d : demoModels())
        if (d.key.compare(key, Qt::CaseInsensitive) == 0) return d.id;
    return std::nullopt;
}

synthetic::ModelDef demoDefinition(DemoModel m) {
    switch (m) {
        case DemoModel::PlateWithHole: return synthetic::plateWithHole(8, 10);
        case DemoModel::CantileverBlock: return synthetic::cantileverBlock(10, 3, 3);
        case DemoModel::TwoBarTrussElastic: return synthetic::twoBarTruss(false);
        case DemoModel::TwoBarTrussPlastic: return synthetic::twoBarTruss(true);
        case DemoModel::SingleQuadPlastic: return synthetic::singleQuad(true);
    }
    return synthetic::plateWithHole(8, 10);
}

std::vector<int> defaultParameterIndices(DemoModel m, const ResultStore& store) {
    std::vector<int> out;
    for (const auto& name : demoInfo(m).defaultParameters)
        for (size_t i = 0; i < store.parameters.size(); ++i)
            if (QString::fromStdString(store.parameters[i].name) == name) out.push_back(static_cast<int>(i));
    return out;
}

QString scratchDirectory() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) base = QDir::tempPath() + "/ABSS";
    const QString s = base + "/scratch";
    QDir().mkpath(s);
    return s;
}

// ---- controller ------------------------------------------------------------------------------------------------

Controller::Controller(QObject* parent) : QObject(parent) {}

Controller::~Controller() {
    if (ctl_) ctl_->cancel = true;
    if (thread_) {
        thread_->wait();
    }
}

QString Controller::displayName() const {
    switch (source_) {
        case Source::Demo: return demoInfo(demo_).title;
        case Source::Inp: return QFileInfo(inpPath_).fileName();
        case Source::Project: return QFileInfo(projectPath_).fileName();
        case Source::None: break;
    }
    return {};
}

void Controller::setModelFromDemo(DemoModel m, std::shared_ptr<ResultStore> s) {
    source_ = Source::Demo;
    demo_ = m;
    inpPath_.clear();
    projectPath_.clear();
    deck_.reset();
    deckDiagnostics_.clear();
    model_ = std::move(s);
    results_.reset();
    stale_ = false;
}

namespace {
std::shared_ptr<ResultStore> prepareDemo(DemoModel m) {
    auto s = std::make_shared<ResultStore>();
    synthetic::SyntheticOracle oracle(demoDefinition(m));
    oracle.runNominal(*s, nullptr);
    return s;
}

struct InpLoad {
    std::shared_ptr<inp::Deck> deck;
    std::shared_ptr<ResultStore> model;
    std::vector<std::string> diagnostics;
};

InpLoad prepareInp(const QString& path) {
    InpLoad r;
    r.deck = std::make_shared<inp::Deck>(inp::Deck::load(path.toStdString()));
    r.model = std::make_shared<ResultStore>();
    r.model->mesh = r.deck->mesh();
    r.model->parameters = inp::discoverParameters(*r.deck);
    r.model->sourceInp = path.toStdString();
    r.model->title = r.deck->heading().empty() ? QFileInfo(path).completeBaseName().toStdString() : r.deck->heading();
    r.model->backend = "abaqus";
    for (const auto& d : r.deck->diagnostics()) {
        const char* lv = d.level == inp::Diagnostic::Level::Error ? "Error" : d.level == inp::Diagnostic::Level::Warning ? "Warning" : "Info";
        r.diagnostics.push_back(std::string(lv) + ": " + QFileInfo(QString::fromStdString(d.file)).fileName().toStdString() +
                                ":" + std::to_string(d.line) + ": " + d.message);
    }
    for (const auto& m : r.deck->missingIncludes()) r.diagnostics.push_back("Missing file: " + m);
    return r;
}
}  // namespace

bool Controller::loadDemo(DemoModel m, QString* error) {
    try {
        setModelFromDemo(m, prepareDemo(m));
        emit modelLoaded();
        emit resultsChanged();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool Controller::loadInp(const QString& path, QString* error) {
    try {
        auto r = prepareInp(path);
        source_ = Source::Inp;
        inpPath_ = path;
        projectPath_.clear();
        deck_ = r.deck;
        deckDiagnostics_ = r.diagnostics;
        model_ = r.model;
        results_.reset();
        stale_ = false;
        emit modelLoaded();
        emit resultsChanged();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool Controller::loadProject(const QString& path, QString* error) {
    try {
        ProjectMeta meta;
        auto s = std::make_shared<ResultStore>(abss::loadProject(path.toStdString(), &meta));
        source_ = Source::Project;
        projectPath_ = path;
        inpPath_ = QString::fromStdString(meta.inpPath);
        deck_.reset();
        deckDiagnostics_.clear();
        model_ = s;
        results_ = s;
        stale_ = false;
        emit modelLoaded();
        emit resultsChanged();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool Controller::saveProject(const QString& path, QString* error) {
    if (!results_) {
        if (error) *error = "There are no results to save yet.";
        return false;
    }
    try {
        ProjectMeta meta;
        meta.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        if (source_ == Source::Inp && !inpPath_.isEmpty()) {
            meta.inpPath = inpPath_.toStdString();
            meta.inpSha256 = sha256File(meta.inpPath);
        }
        meta.solverBackend = results_->backend;
        meta.computeBackend = par::backendDescription();
        QJsonObject o;
        QJsonArray ps;
        for (int p : lastSettings_.parameters) ps.append(p);
        o["parameters"] = ps;
        o["maxOrder"] = lastSettings_.maxOrder;
        o["validate"] = lastSettings_.validate;
        o["tangentCheck"] = lastSettings_.tangentCheck;
        o["relativeStep"] = lastSettings_.relativeStep;
        QJsonArray outs;
        for (const auto& f : lastSettings_.outputs) outs.append(QString::fromStdString(f));
        o["outputs"] = outs;
        if (source_ == Source::Demo) o["demo"] = demoInfo(demo_).key;
        meta.settingsJson = QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
        abss::saveProject(path.toStdString(), *results_, meta);
        projectPath_ = path;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

namespace {
/// Run `work` on a new thread; `done` runs on the GUI thread (context `ctx`) afterwards.
void inBackground(QObject* ctx, std::function<void()> work, std::function<void()> done) {
    QThread* t = QThread::create(std::move(work));
    QObject::connect(t, &QThread::finished, ctx, [t, done = std::move(done)]() {
        done();
        t->deleteLater();
    });
    t->start();
}
}  // namespace

void Controller::loadDemoAsync(DemoModel m) {
    emit busyChanged(true, "Preparing the demo model…");
    auto result = std::make_shared<std::shared_ptr<ResultStore>>();
    auto err = std::make_shared<QString>();
    inBackground(
        this,
        [m, result, err]() {
            try {
                *result = prepareDemo(m);
            } catch (const std::exception& e) {
                *err = QString::fromUtf8(e.what());
            }
        },
        [this, m, result, err]() {
            emit busyChanged(false, {});
            if (!*result) {
                emit loadFailed("The demo model could not be prepared", *err);
                return;
            }
            setModelFromDemo(m, *result);
            emit modelLoaded();
            emit resultsChanged();
        });
}

void Controller::loadInpAsync(const QString& path) {
    emit busyChanged(true, "Reading " + QFileInfo(path).fileName() + "…");
    auto result = std::make_shared<InpLoad>();
    auto err = std::make_shared<QString>();
    inBackground(
        this,
        [path, result, err]() {
            try {
                *result = prepareInp(path);
            } catch (const std::exception& e) {
                *err = QString::fromUtf8(e.what());
            }
        },
        [this, path, result, err]() {
            emit busyChanged(false, {});
            if (!result->deck) {
                emit loadFailed("This input file could not be read", *err);
                return;
            }
            source_ = Source::Inp;
            inpPath_ = path;
            projectPath_.clear();
            deck_ = result->deck;
            deckDiagnostics_ = result->diagnostics;
            model_ = result->model;
            results_.reset();
            stale_ = false;
            emit modelLoaded();
            emit resultsChanged();
        });
}

void Controller::loadProjectAsync(const QString& path) {
    emit busyChanged(true, "Opening " + QFileInfo(path).fileName() + "…");
    auto result = std::make_shared<std::shared_ptr<ResultStore>>();
    auto meta = std::make_shared<ProjectMeta>();
    auto err = std::make_shared<QString>();
    inBackground(
        this,
        [path, result, meta, err]() {
            try {
                *result = std::make_shared<ResultStore>(abss::loadProject(path.toStdString(), meta.get()));
            } catch (const std::exception& e) {
                *err = QString::fromUtf8(e.what());
            }
        },
        [this, path, result, meta, err]() {
            emit busyChanged(false, {});
            if (!*result) {
                emit loadFailed("This results file could not be opened", *err);
                return;
            }
            source_ = Source::Project;
            projectPath_ = path;
            inpPath_ = QString::fromStdString(meta->inpPath);
            deck_.reset();
            deckDiagnostics_.clear();
            model_ = *result;
            results_ = *result;
            stale_ = false;
            emit modelLoaded();
            emit resultsChanged();
        });
}

void Controller::markSetupChanged() {
    if (results_ && source_ != Source::Project) stale_ = true;
}

namespace {
void applyNames(const ResultStore& from, ResultStore& to) {
    for (auto& p : to.parameters)
        for (const auto& q : from.parameters)
            if (q.id == p.id) p.name = q.name;
}
}  // namespace

void Controller::setParameterName(int index, const QString& name) {
    if (!model_ || index < 0 || static_cast<size_t>(index) >= model_->parameters.size() || name.trimmed().isEmpty()) return;
    model_->parameters[static_cast<size_t>(index)].name = name.trimmed().toStdString();
    if (results_ && results_ != model_) applyNames(*model_, *results_);
    emit resultsChanged();
}

CostEstimate Controller::estimate(const RunSettings& s) const {
    CostEstimate c;
    if (!model_) return c;
    const int frames = model_->frames.empty() ? 0 : static_cast<int>(model_->frames.size()) - 1;
    const int P = s.maxOrder > 0 ? static_cast<int>(s.parameters.size()) : 0;
    const int K = std::max(1, s.maxOrder);
    c.increments = frames;
    c.parameters = P;
    c.halfWidth = defaultHalfWidth(K);
    const bool inp = source_ == Source::Inp;
    if (s.maxOrder > 0 && P > 0) {
        c.engineEvaluations = estimateOracleEvaluations(std::max(frames, 1), P, K, c.halfWidth, s.tangentCheck);
        if (s.validate) c.validationAnalyses = 1 + static_cast<long long>(P) * 2 * c.halfWidth;
    }
    // Storage: every kept field, nominal + P*K derivative series, all increments.
    double perFrame = 0;
    for (size_t f = 0; f < model_->fields.size(); ++f) {
        const auto& fd = model_->fields[f];
        if (!s.outputs.empty() && std::find(s.outputs.begin(), s.outputs.end(), fd.name) == s.outputs.end()) continue;
        perFrame += double(model_->locationCount(static_cast<int>(f))) * fd.componentCount() * 8.0;
    }
    c.storageBytes = perFrame * (frames + 1) * (1 + double(P) * (s.maxOrder > 0 ? K : 0));

    auto num = [](double v) { return QLocale().toString(v, 'f', 0); };
    if (inp)
        c.lines << "Increments: known after the nominal Abaqus analysis (every accepted increment is used)";
    else
        c.lines << QString("Increments: %1 (every accepted increment)").arg(frames);
    c.lines << QString("Parameters: %1 · derivative order: %2").arg(P).arg(s.maxOrder);
    if (s.maxOrder == 0 || P == 0) {
        c.lines << "Work: the nominal analysis only";
    } else if (inp) {
        c.lines << QString("Model evaluations: about %1 per increment (residual probes and branch commits)")
                       .arg(num(double(estimateOracleEvaluations(1, P, K, c.halfWidth, s.tangentCheck))));
    } else {
        c.lines << QString("Model evaluations: about %1 (residual probes and branch commits)").arg(num(double(c.engineEvaluations)));
    }
    if (c.validationAnalyses > 0)
        c.lines << QString("Validation: %1 complete re-analyses (each covers every increment)").arg(c.validationAnalyses);
    if (!inp) c.lines << QString("Result size: about %1").arg(QLocale().formattedDataSize(static_cast<qint64>(c.storageBytes)));
    c.lines << (inp ? "Runs on: Abaqus (licence-limited, one job at a time)" : "Runs on: synthetic demo model (no Abaqus needed)");
    if (s.maxOrder >= 3) c.warnings << "Third order is experimental: results are marked Experimental everywhere they appear.";
    if (s.maxOrder >= 2) c.warnings << "Each extra order adds two branch analyses per parameter.";
    if (s.maxOrder > 0 && P == 0) c.warnings << "Select at least one parameter, or choose order 0 (nominal only).";
    return c;
}

std::shared_ptr<ResultStore> Controller::filterFields(const ResultStore& in, const std::vector<std::string>& keep) {
    auto out = std::make_shared<ResultStore>();
    out->title = in.title;
    out->sourceInp = in.sourceInp;
    out->backend = in.backend;
    out->synthetic = in.synthetic;
    out->mesh = in.mesh;
    out->points = in.points;
    out->frames = in.frames;
    out->parameters = in.parameters;
    out->quality = in.quality;
    out->log = in.log;
    std::vector<int> map(in.fields.size(), -1);
    for (size_t f = 0; f < in.fields.size(); ++f)
        if (keep.empty() || std::find(keep.begin(), keep.end(), in.fields[f].name) != keep.end()) {
            map[f] = static_cast<int>(out->fields.size());
            out->fields.push_back(in.fields[f]);
        }
    in.forEachSeries([&](int f, const DerivativeSpec& d, int frame, const std::vector<double>& v) {
        if (map[static_cast<size_t>(f)] >= 0) out->set(map[static_cast<size_t>(f)], d, frame, v);
    });
    return out;
}

RunOutcome Controller::executeRun(Source src, DemoModel demo, const QString& inpPath, std::shared_ptr<const ResultStore> model,
                                  const RunSettings& s, RunControl& ctl) {
    RunOutcome out;
    auto store = std::make_shared<ResultStore>();
    std::vector<std::string> logLines;
    auto outerLog = ctl.log;
    auto outerProgress = ctl.progress;
    ctl.log = [&](const std::string& l) {
        logLines.push_back(l);
        if (outerLog) outerLog(l);
    };
    auto phase = [&](double base, double span) {
        ctl.progress = [outerProgress, base, span](const std::string& stage, double f) {
            if (outerProgress) outerProgress(stage, base + span * std::clamp(f, 0.0, 1.0));
        };
    };
    try {
        std::unique_ptr<Oracle> oracle;
        if (src == Source::Demo) {
            oracle = std::make_unique<synthetic::SyntheticOracle>(demoDefinition(demo));
            ctl.log("Backend: synthetic demo model (analytic stand-in for Abaqus; results are labelled synthetic).");
        } else if (src == Source::Inp) {
            AbaqusRunConfig cfg;
            cfg.inpPath = inpPath.toStdString();
            const auto inst = detectAbaqus(s.abaqusCommand);
            if (!inst.found)
                throw std::runtime_error("Abaqus was not found on this computer. " + inst.message +
                                         " Abaqus is needed to run new analyses of an input file; the demo models run without it.");
            cfg.abaqusCommand = inst.command;
            cfg.scratchDir = scratchDirectory().toStdString();
            if (model) cfg.parameters = model->parameters;
            cfg.log = ctl.log;
            cfg.cancel = &ctl.cancel;
            oracle = makeAbaqusOracle(cfg);
        } else {
            throw std::runtime_error("These results were opened from a project file. To compute new results, open the "
                                     "model's input file or a demo model.");
        }
        phase(0.0, 0.12);
        if (ctl.progress) ctl.progress("Nominal analysis", 0.0);
        const auto u = oracle->runNominal(*store, ctl.progress);
        ctl.log("Nominal analysis: " + std::to_string(store->frames.size() - 1) + " increments.");
        bool cancelled = ctl.cancel.load();
        std::string summary;
        if (!cancelled && s.maxOrder > 0 && !s.parameters.empty()) {
            SensitivityRequest req;
            req.parameters = s.parameters;
            req.maxOrder = s.maxOrder;
            req.tangentCheck = s.tangentCheck;
            if (s.relativeStep > 0)
                for (int p : s.parameters) {
                    const double a = std::abs(store->parameters.at(static_cast<size_t>(p)).nominal);
                    req.absoluteSteps.push_back(s.relativeStep * (a > 0 ? a : 1.0));
                }
            phase(0.12, s.validate ? 0.55 : 0.88);
            const auto rep = runSensitivities(*oracle, u, req, *store, ctl);
            cancelled = rep.cancelled || ctl.cancel.load();
            ctl.log("Sensitivity engine: " + std::to_string(rep.oracleEvaluations) + " model evaluations, compute backend " +
                    rep.computeBackend + ".");
            if (!cancelled && s.validate) {
                phase(0.67, 0.33);
                ValidationOptions vo;
                summary = validateAgainstReruns(*oracle, req, *store, vo, ctl);
                ctl.log("Validation against full re-analyses:\n" + summary);
                cancelled = ctl.cancel.load();
            }
        }
        if (outerProgress) outerProgress(cancelled ? "Stopped" : "Finished", 1.0);
        for (const auto& l : logLines) store->log.push_back(l);
        out.store = s.outputs.empty() ? store : filterFields(*store, s.outputs);
        out.cancelled = cancelled;
        out.ok = !cancelled;
        out.message = cancelled ? "The run was stopped. Results computed so far are shown; incomplete series are marked "
                                  "\"No data\"."
                                : "Results are ready.";
        out.detail = QString::fromStdString(summary);
    } catch (const std::exception& e) {
        out.ok = false;
        out.message = QString::fromUtf8(e.what());
        out.detail.clear();
        for (const auto& l : logLines) out.detail += QString::fromStdString(l) + "\n";
    }
    ctl.log = outerLog;
    ctl.progress = outerProgress;
    return out;
}

bool Controller::runSync(const RunSettings& s, QString* error) {
    RunControl ctl;
    auto o = executeRun(source_, demo_, inpPath_, model_, s, ctl);
    if (!o.ok || !o.store) {
        if (error) *error = o.message;
        return false;
    }
    lastSettings_ = s;
    results_ = o.store;
    if (model_) applyNames(*model_, *results_);
    stale_ = false;
    emit resultsChanged();
    return true;
}

void Controller::startRun(const RunSettings& s) {
    if (running_) return;
    running_ = true;
    lastSettings_ = s;
    ctl_ = std::make_shared<RunControl>();
    auto ctl = ctl_;
    QPointer<Controller> self(this);
    ctl->progress = [self](const std::string& stage, double f) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, st = QString::fromStdString(stage), f]() {
            if (self) emit self->runProgress(st, f);
        }, Qt::QueuedConnection);
    };
    ctl->log = [self](const std::string& line) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, l = QString::fromStdString(line)]() {
            if (self) emit self->runLog(l);
        }, Qt::QueuedConnection);
    };
    auto outcome = std::make_shared<RunOutcome>();
    const auto src = source_;
    const auto demo = demo_;
    const auto inp = inpPath_;
    std::shared_ptr<const ResultStore> model = model_;
    emit runStarted();
    thread_ = QThread::create([=]() { *outcome = executeRun(src, demo, inp, model, s, *ctl); });
    connect(thread_, &QThread::finished, this, [this, outcome]() {
        thread_->deleteLater();
        thread_ = nullptr;
        running_ = false;
        if (outcome->store && (outcome->ok || outcome->cancelled)) {
            results_ = outcome->store;
            if (model_) applyNames(*model_, *results_);
            stale_ = false;
            emit resultsChanged();
        }
        emit runFinished(outcome->ok, outcome->cancelled, outcome->message, outcome->detail);
    });
    thread_->start();
}

void Controller::cancelRun() {
    if (ctl_) ctl_->cancel = true;
}

// ---- exports ---------------------------------------------------------------------------------------------------

std::vector<double> historyAxis(const ResultStore& store, HistoryAxis axis, HistoryAxis* used) {
    std::vector<double> x(store.frames.size());
    HistoryAxis u = axis;
    if (axis == HistoryAxis::TotalTime) {
        bool mono = true;
        for (size_t i = 1; i < store.frames.size(); ++i)
            if (!(store.frames[i].totalTime > store.frames[i - 1].totalTime)) mono = false;
        if (!mono) u = HistoryAxis::Increment;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        const auto& f = store.frames[i];
        x[i] = u == HistoryAxis::TotalTime ? f.totalTime : u == HistoryAxis::StepTime ? f.stepTime : double(i);
    }
    if (used) *used = u;
    return x;
}

namespace {
QString csvQuote(const QString& s) {
    if (s.contains(',') || s.contains('"') || s.contains('\n')) {
        QString q = s;
        q.replace("\"", "\"\"");
        return "\"" + q + "\"";
    }
    return s;
}
QString csvNum(double v) { return v == v ? QString::number(v, 'g', 17) : QString(); }
QString letter(size_t i) { return i < 26 ? QString(QChar('A' + static_cast<int>(i))) : QString("P%1").arg(i + 1); }

QString commonHeader(const ResultStore& store) {
    QString h = "# ABSS export\n";
    h += "# Model: " + QString::fromStdString(store.title) + "\n";
    if (store.synthetic) h += "# SYNTHETIC DEMO DATA - computed from an analytic model, not from Abaqus\n";
    h += "# Backend: " + QString::fromStdString(store.backend) + "\n";
    return h;
}

QString qualityWord(const ResultStore& store, const DerivativeSpec& d) {
    if (d.order() == 0) return "nominal";
    auto it = store.quality.find(d);
    return it == store.quality.end() ? "no quality record" : QString(qualityName(it->second.status));
}
}  // namespace

QString historyCsv(const ResultStore& store, const std::vector<viz::PickTarget>& picks, const std::vector<viz::DisplaySpec>& series) {
    QString out = commonHeader(store);
    for (const auto& s : series) {
        const auto dv = viz::extractDisplay(store, s, 0);
        out += "# Series: " + QString::fromStdString(viz::notationLabel(store, s.output, s.derivative)) +
               " | units: " + QString::fromStdString(viz::unitsLine(store, s.output, s.derivative)) +
               " | quality: " + qualityWord(store, s.derivative);
        if (!dv.projectionNote.empty()) out += " | " + QString::fromStdString(dv.projectionNote);
        out += "\n";
    }
    for (size_t p = 0; p < picks.size(); ++p)
        out += "# Point " + letter(p) + ": " + QString::fromStdString(viz::describeTarget(store, picks[p])) + "\n";
    QStringList head = {"frame", "step", "increment", "step_time", "total_time"};
    std::vector<std::vector<double>> cols;
    for (size_t p = 0; p < picks.size(); ++p)
        for (const auto& s : series) {
            head << csvQuote(letter(p) + ": " + QString::fromStdString(viz::notationLabel(store, s.output, s.derivative)) + " at " +
                             QString::fromStdString(viz::describeTarget(store, picks[p])));
            cols.push_back(viz::historyAt(store, s, picks[p]));
        }
    out += head.join(',') + "\n";
    for (size_t f = 0; f < store.frames.size(); ++f) {
        const auto& fr = store.frames[f];
        QStringList row = {QString::number(f), csvQuote(QString::fromStdString(fr.stepName)), QString::number(fr.increment),
                           csvNum(fr.stepTime), csvNum(fr.totalTime)};
        for (const auto& c : cols) row << csvNum(c[f]);
        out += row.join(',') + "\n";
    }
    return out;
}

QString frameCsv(const ResultStore& store, const viz::DisplaySpec& spec, int frame) {
    QString out = commonHeader(store);
    const auto dv = viz::extractDisplay(store, spec, frame);
    out += "# Quantity: " + QString::fromStdString(viz::notationLabel(store, spec.output, spec.derivative)) + " | units: " +
           QString::fromStdString(viz::unitsLine(store, spec.output, spec.derivative)) + " | quality: " +
           qualityWord(store, spec.derivative) + "\n";
    out += "# " + QString::fromStdString(viz::frameLabel(store, frame)) + "\n";
    if (!dv.projectionNote.empty()) out += "# " + QString::fromStdString(dv.projectionNote) + "\n";
    const bool nodes = dv.assoc == viz::DataAssoc::Point;
    out += nodes ? "node_label,instance,value\n" : "element_label,instance,value\n";
    for (size_t i = 0; i < dv.values.size(); ++i) {
        std::int64_t label;
        std::int32_t inst;
        if (nodes) {
            label = store.mesh.nodeKeys[i].label;
            inst = store.mesh.nodeKeys[i].instance;
        } else {
            label = store.mesh.elements[i].label;
            inst = store.mesh.elements[i].instance;
        }
        const QString iname = inst >= 0 && static_cast<size_t>(inst) < store.mesh.instanceNames.size()
                                  ? QString::fromStdString(store.mesh.instanceNames[static_cast<size_t>(inst)])
                                  : QString();
        out += QString::number(label) + "," + csvQuote(iname) + "," + csvNum(dv.values[i]) + "\n";
    }
    return out;
}

}  // namespace abss::gui
