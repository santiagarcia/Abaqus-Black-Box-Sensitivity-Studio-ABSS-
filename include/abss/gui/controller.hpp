// ABSS GUI controller: application state behind the widgets (model source, discovered parameters, run settings,
// background runs, results, persistence, exports). Free of widgets so it can be tested headless.
#pragma once

#include "abss/core/project_io.hpp"
#include "abss/core/results.hpp"
#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"
#include "abss/viz/display.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QThread;

namespace abss::inp {
class Deck;
}

namespace abss::gui {

enum class DemoModel { PlateWithHole, CantileverBlock, TwoBarTrussElastic, TwoBarTrussPlastic, SingleQuadPlastic };

struct DemoInfo {
    DemoModel id;
    QString key;          ///< command-line key: plate, cantilever, truss, truss-plastic, quad
    QString title;
    QString description;
    QStringList defaultParameters;  ///< parameter names preselected on the Parameters page
};
/// Indices of a demo's default parameters within a loaded store.
std::vector<int> defaultParameterIndices(DemoModel m, const ResultStore& store);
/// Application data folders (QStandardPaths::AppLocalDataLocation): scratch for Abaqus jobs, logs.
QString scratchDirectory();
const std::vector<DemoInfo>& demoModels();
const DemoInfo& demoInfo(DemoModel m);
std::optional<DemoModel> demoFromKey(const QString& key);
synthetic::ModelDef demoDefinition(DemoModel m);

/// Everything the user chose on the Set up page.
struct RunSettings {
    std::vector<int> parameters;   ///< indices into model()->parameters
    int maxOrder = 1;              ///< 0 = nominal only
    bool validate = true;          ///< compare against full re-equilibrated reruns afterwards
    bool tangentCheck = true;
    double relativeStep = 0.0;     ///< 0 = each parameter's default
    std::vector<std::string> outputs;  ///< field names to keep (empty = all)
    std::string abaqusCommand;     ///< user override for the Abaqus launcher (empty = auto-detect)
};

struct CostEstimate {
    int increments = 0;
    int parameters = 0;
    int halfWidth = 0;
    long long engineEvaluations = 0;  ///< from abss::estimateOracleEvaluations
    long long validationAnalyses = 0; ///< full reruns used by validation (each covers every increment)
    double storageBytes = 0;          ///< in-memory/disk size of the result arrays
    QStringList lines;                ///< ready-to-display text
    QStringList warnings;
};

/// Outcome of a run executed on any thread.
struct RunOutcome {
    std::shared_ptr<ResultStore> store;
    bool ok = false;
    bool cancelled = false;
    QString message;       ///< plain-language result/error
    QString detail;        ///< technical detail / validation summary
};

class Controller : public QObject {
    Q_OBJECT
public:
    enum class Source { None, Demo, Inp, Project };

    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    Source source() const { return source_; }
    DemoModel demoModel() const { return demo_; }
    QString inpPath() const { return inpPath_; }
    QString projectPath() const { return projectPath_; }
    /// Model shown before a run: mesh, parameters, increments (demo: nominal already computed). Null when none.
    std::shared_ptr<const ResultStore> model() const { return model_; }
    /// Latest results (demo run, opened project). Null when none.
    std::shared_ptr<const ResultStore> results() const { return results_; }
    const inp::Deck* deck() const { return deck_.get(); }
    const std::vector<std::string>& deckDiagnostics() const { return deckDiagnostics_; }
    bool running() const { return running_; }
    bool resultsStale() const { return stale_; }
    const RunSettings& lastRunSettings() const { return lastSettings_; }
    QString displayName() const;

    // ---- synchronous operations (tests, worker threads, command line) ----
    bool loadDemo(DemoModel m, QString* error = nullptr);
    bool loadInp(const QString& path, QString* error = nullptr);
    bool loadProject(const QString& path, QString* error = nullptr);
    bool saveProject(const QString& path, QString* error = nullptr);
    bool runSync(const RunSettings& s, QString* error = nullptr);

    // ---- asynchronous operations (GUI); completion via signals ----
    void loadDemoAsync(DemoModel m);
    void loadInpAsync(const QString& path);
    void loadProjectAsync(const QString& path);
    void startRun(const RunSettings& s);
    void cancelRun();

    CostEstimate estimate(const RunSettings& s) const;
    /// Mark results as stale after the setup changed (results are kept).
    void markSetupChanged();
    /// Rename a parameter (display name); applied to the model and to results with the same parameter id.
    void setParameterName(int index, const QString& name);

    /// Executes a run (any thread). Uses its own oracle; never touches controller state.
    static RunOutcome executeRun(Source src, DemoModel demo, const QString& inpPath,
                                 std::shared_ptr<const ResultStore> model, const RunSettings& s, RunControl& ctl);
    /// Copy of `in` with only the named fields (all series, quality, frames and metadata preserved).
    static std::shared_ptr<ResultStore> filterFields(const ResultStore& in, const std::vector<std::string>& keep);

signals:
    void modelLoaded();
    void loadFailed(const QString& title, const QString& detail);
    void resultsChanged();
    void runStarted();
    void runProgress(const QString& stage, double fraction);
    void runLog(const QString& line);
    void runFinished(bool ok, bool cancelled, const QString& message, const QString& detail);
    void busyChanged(bool busy, const QString& what);

private:
    void setModelFromDemo(DemoModel m, std::shared_ptr<ResultStore> s);

    Source source_ = Source::None;
    DemoModel demo_ = DemoModel::PlateWithHole;
    QString inpPath_, projectPath_;
    std::shared_ptr<ResultStore> model_;
    std::shared_ptr<ResultStore> results_;
    std::shared_ptr<inp::Deck> deck_;
    std::vector<std::string> deckDiagnostics_;
    RunSettings lastSettings_;
    bool running_ = false;
    bool stale_ = false;
    std::shared_ptr<RunControl> ctl_;
    QThread* thread_ = nullptr;
};

// ---- exports ---------------------------------------------------------------------------------------------------

enum class HistoryAxis { TotalTime, StepTime, Increment };
/// X values for history plots/CSV; TotalTime falls back to Increment when total time is not monotonic.
std::vector<double> historyAxis(const ResultStore& store, HistoryAxis axis, HistoryAxis* used = nullptr);

/// CSV of histories: comment header (title, synthetic marker, projection, units), then frame, step, increment,
/// step time, total time and one column per (pick, series). Excel-friendly (comma separated, '.' decimals).
QString historyCsv(const ResultStore& store, const std::vector<viz::PickTarget>& picks,
                   const std::vector<viz::DisplaySpec>& series);

/// CSV of the displayed values at one increment (one row per node or element).
QString frameCsv(const ResultStore& store, const viz::DisplaySpec& spec, int frame);

}  // namespace abss::gui
