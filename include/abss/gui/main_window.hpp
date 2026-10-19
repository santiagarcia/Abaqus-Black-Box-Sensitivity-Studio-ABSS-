// ABSS GUI: the single application window (step rail + pages + status bar) and command-line automation.
#pragma once

#include "abss/gui/controller.hpp"
#include "abss/oracle/abaqus.hpp"

#include <QMainWindow>

#include <functional>
#include <optional>

class QLabel;
class QListWidget;
class QStackedWidget;
class QAction;

namespace abss::gui {

class StartPage;
class ParamsPage;
class SetupPage;
class RunPage;
class ExplorePage;

/// Options for unattended use (screenshots, scripted demos): see apps/abss/main.cpp --help.
struct Automation {
    QString demoKey;      ///< open this bundled demo
    QString openPath;     ///< open this .inp or .abss
    bool run = false;     ///< start the run with the page defaults once the model is loaded
    int order = -1;       ///< derivative order override for the run (-1 = default)
    QStringList params;   ///< parameters to select (exact display name, or name ending in ": <token>")
    QString output;       ///< select this output on the Results page, e.g. "U2", "S_Mises"
    int resultOrder = -1; ///< order to display on the Results page
    int frame = -1;       ///< increment to display (-1 = last)
    int picks = 0;        ///< auto-pick this many points for the history chart
    QString page;         ///< page to show before the screenshot: start, parameters, setup, run, results
    QString screenshot;   ///< save a PNG of the whole window here, then quit
    QString viewShot;     ///< also save the 3D view alone here
    QString chartShot;    ///< also save the history chart here
    QString saveProject;  ///< save results to this .abss after the run
    bool quit = false;    ///< quit after the automation finished
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    Controller* controller() { return controller_; }
    ExplorePage* explorePage() { return explore_; }
    void goTo(int page);
    void openPath(const QString& path);
    void startAutomation(const Automation& a);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    enum Page { PStart = 0, PParams, PSetup, PRun, PResults };
    void buildMenus();
    void updateRail();
    void updateTitle();
    void updateRunEnable();
    void detectAbaqusAsync();
    void startRun();
    void chooseFile();
    void saveProjectAs();
    void saveProject();
    void setTheme(int mode);
    void showVerified();
    void finishAutomation();

    Controller* controller_;
    QListWidget* rail_;
    QStackedWidget* stack_;
    StartPage* start_;
    ParamsPage* params_;
    SetupPage* setup_;
    RunPage* run_;
    ExplorePage* explore_;
    QLabel* abqStatus_;
    QLabel* computeStatus_;
    QLabel* readout_;
    std::optional<AbaqusInstallation> abaqus_;
    bool detecting_ = false;
    bool pendingRun_ = false;
    std::optional<Automation> auto_;
    QAction* saveAct_ = nullptr;
    QAction* saveAsAct_ = nullptr;
    QList<QAction*> resultActions_;
    QList<QAction*> pickActions_;
    QAction* runAct_ = nullptr;
    QAction* cancelAct_ = nullptr;
};

}  // namespace abss::gui
