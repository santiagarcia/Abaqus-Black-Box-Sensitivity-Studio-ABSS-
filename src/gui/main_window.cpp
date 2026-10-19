#include "abss/gui/main_window.hpp"

#include "abss/gui/explore_page.hpp"
#include "abss/gui/pages.hpp"
#include "abss/gui/theme.hpp"
#include "abss/linalg/parallel.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

namespace abss::gui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), controller_(new Controller(this)) {
    setWindowIcon(appIcon());
    setMinimumSize(1100, 720);
    resize(1440, 900);
    auto* central = new QWidget;
    auto* h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    rail_ = new QListWidget;
    rail_->setObjectName("rail");
    rail_->setFixedWidth(150);
    const QStringList names = {"Start", "Parameters", "Set up", "Run", "Results"};
    const QStyle::StandardPixmap icons[] = {QStyle::SP_DirHomeIcon, QStyle::SP_FileDialogDetailedView, QStyle::SP_FileDialogContentsView,
                                            QStyle::SP_MediaPlay, QStyle::SP_DesktopIcon};
    for (int i = 0; i < names.size(); ++i) {
        auto* it = new QListWidgetItem(style()->standardIcon(icons[i]), QString("%1   %2").arg(i + 1).arg(names[i]));
        rail_->addItem(it);
    }
    h->addWidget(rail_);
    stack_ = new QStackedWidget;
    start_ = new StartPage(controller_);
    params_ = new ParamsPage(controller_);
    setup_ = new SetupPage(controller_);
    run_ = new RunPage(controller_);
    explore_ = new ExplorePage(controller_);
    for (QWidget* w : std::initializer_list<QWidget*>{start_, params_, setup_, run_, explore_}) stack_->addWidget(w);
    h->addWidget(stack_, 1);
    setCentralWidget(central);

    abqStatus_ = new QLabel("Abaqus: checking…");
    computeStatus_ = new QLabel("Compute: " + qs(par::backendDescription()));
    readout_ = new QLabel;
    statusBar()->addWidget(abqStatus_);
    statusBar()->addWidget(computeStatus_);
    statusBar()->addWidget(readout_, 1);

    buildMenus();

    connect(rail_, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0 && stack_->currentIndex() != r) stack_->setCurrentIndex(r);
    });
    connect(start_, &StartPage::chooseFileRequested, this, &MainWindow::chooseFile);
    connect(start_, &StartPage::openFileRequested, this, &MainWindow::openPath);
    connect(start_, &StartPage::demoRequested, this, [this](DemoModel m, bool runNow) {
        pendingRun_ = runNow;
        controller_->loadDemoAsync(m);
    });
    connect(start_, &StartPage::locateAbaqusRequested, this, [this]() {
        const QString p = QFileDialog::getOpenFileName(this, "Locate the Abaqus launcher", "C:/SIMULIA/Commands",
                                                       "Abaqus launcher (abaqus*.bat abq*.bat abaqus*.exe);;All files (*)");
        if (p.isEmpty()) return;
        QSettings().setValue("abaqusCommand", p);
        detectAbaqusAsync();
    });
    connect(start_, &StartPage::licenseCheckRequested, this, [this]() {
        if (!abaqus_ || !abaqus_->found) return;
        start_->setLicenseChecking();
        auto inst = std::make_shared<AbaqusInstallation>(*abaqus_);
        QThread* t = QThread::create([inst]() {
            try {
                checkAbaqusLicense(*inst, scratchDirectory().toStdString());
            } catch (const std::exception& e) {
                inst->licenseChecked = true;
                inst->licenseOk = false;
                inst->message = e.what();
            }
        });
        connect(t, &QThread::finished, this, [this, t, inst]() {
            abaqus_ = *inst;
            start_->setLicenseResult(*inst);
            t->deleteLater();
        });
        t->start();
    });
    connect(params_, &ParamsPage::selectionChanged, this, [this]() {
        setup_->setParameters(params_->selected());
        controller_->markSetupChanged();
        updateRail();
    });
    connect(params_, &ParamsPage::nextRequested, this, [this]() { goTo(PSetup); });
    connect(setup_, &SetupPage::settingsChanged, this, [this]() {
        controller_->markSetupChanged();
        updateRunEnable();
    });
    connect(setup_, &SetupPage::nextRequested, this, [this]() { goTo(PRun); });
    connect(run_, &RunPage::startRequested, this, &MainWindow::startRun);
    connect(run_, &RunPage::cancelRequested, controller_, &Controller::cancelRun);
    connect(run_, &RunPage::openResultsRequested, this, [this]() { goTo(PResults); });
    connect(explore_, &ExplorePage::statusMessage, readout_, &QLabel::setText);
    connect(explore_, &ExplorePage::exportsChanged, this, [this]() {
        for (auto* a : pickActions_) a->setEnabled(explore_->hasPicks());
    });

    connect(controller_, &Controller::busyChanged, this, [this](bool busy, const QString& what) {
        start_->setBusy(busy, what);
        if (busy) readout_->setText(what);
        else readout_->clear();
    });
    connect(controller_, &Controller::loadFailed, this, [this](const QString& t, const QString& d) {
        pendingRun_ = false;
        start_->showLoadError(t, d);
        goTo(PStart);
        if (auto_ && auto_->quit) QTimer::singleShot(200, qApp, &QApplication::quit);
    });
    connect(controller_, &Controller::modelLoaded, this, [this]() {
        params_->reload();
        setup_->reload();
        if (auto_ && !auto_->params.isEmpty() && controller_->model()) {
            std::vector<int> sel;
            const auto& ps = controller_->model()->parameters;
            for (const auto& want : auto_->params)
                for (size_t i = 0; i < ps.size(); ++i) {
                    const QString n = QString::fromStdString(ps[i].name);
                    if (n.compare(want, Qt::CaseInsensitive) == 0 || n.endsWith(": " + want, Qt::CaseInsensitive)) {
                        sel.push_back(static_cast<int>(i));
                        break;
                    }
                }
            params_->setSelected(sel);
        }
        setup_->setParameters(params_->selected());
        if (auto_ && auto_->order >= 0) setup_->setOrder(auto_->order);
        updateRail();
        updateTitle();
        updateRunEnable();
        if (controller_->source() == Controller::Source::Project) {
            goTo(PResults);
        } else if (pendingRun_) {
            pendingRun_ = false;
            goTo(PRun);
            startRun();
        } else {
            goTo(PParams);
        }
        if (auto_ && !auto_->run && controller_->source() != Controller::Source::Project)
            QTimer::singleShot(1500, this, &MainWindow::finishAutomation);
        if (auto_ && controller_->source() == Controller::Source::Project) QTimer::singleShot(1500, this, &MainWindow::finishAutomation);
    });
    connect(controller_, &Controller::resultsChanged, this, [this]() {
        auto r = controller_->results();
        if (r != nullptr || explore_->hasStore()) explore_->setStore(r);
        explore_->setStaleBanner(controller_->resultsStale());
        updateRail();
        updateTitle();
    });
    connect(controller_, &Controller::runStarted, this, [this]() {
        run_->onStarted();
        params_->setLocked(true);
        setup_->setLocked(true);
        runAct_->setEnabled(false);
        cancelAct_->setEnabled(true);
        updateRail();
    });
    connect(controller_, &Controller::runProgress, run_, &RunPage::onProgress);
    connect(controller_, &Controller::runLog, run_, &RunPage::onLog);
    connect(controller_, &Controller::runFinished, this, [this](bool ok, bool cancelled, const QString& msg, const QString& detail) {
        run_->onFinished(ok, cancelled, msg, detail);
        params_->setLocked(false);
        setup_->setLocked(false);
        cancelAct_->setEnabled(false);
        updateRunEnable();
        updateRail();
        if ((ok || cancelled) && stack_->currentIndex() == PRun) goTo(PResults);
        if (auto_) QTimer::singleShot(ok ? 900 : 300, this, &MainWindow::finishAutomation);
    });
    connect(stack_, &QStackedWidget::currentChanged, this, [this](int i) {
        rail_->blockSignals(true);
        rail_->setCurrentRow(i);
        rail_->blockSignals(false);
    });

    for (int i = 0; i < 5; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        connect(sc, &QShortcut::activated, this, [this, i]() { goTo(i); });
    }

    const int theme = QSettings().value("theme", 0).toInt();
    if (theme != 0) setTheme(theme);
    updateRail();
    updateTitle();
    updateRunEnable();
    goTo(PStart);
    detectAbaqusAsync();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu("&File");
    auto* open = file->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "&Open input file or results…", this, &MainWindow::chooseFile);
    open->setShortcut(QKeySequence::Open);
    auto* demo = file->addAction("Open &demo…", this, [this]() { goTo(PStart); });
    demo->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    file->addSeparator();
    saveAct_ = file->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "&Save results", this, &MainWindow::saveProject);
    saveAct_->setShortcut(QKeySequence::Save);
    saveAsAct_ = file->addAction("Save results &as…", this, &MainWindow::saveProjectAs);
    resultActions_ << saveAct_ << saveAsAct_;
    auto* exp = file->addMenu("&Export");
    auto* shot = exp->addAction("Screenshot of the 3D view (PNG)…", explore_, &ExplorePage::exportScreenshot);
    shot->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    auto* fcsv = exp->addAction("Values at this increment (CSV)…", explore_, &ExplorePage::exportFrameCsv);
    auto* vtu = exp->addAction("This increment for ParaView (VTU)…", explore_, &ExplorePage::exportVtu);
    resultActions_ << shot << fcsv << vtu;
    auto* hcsv = exp->addAction("History at picked points (CSV)…", explore_, &ExplorePage::exportHistoryCsv);
    auto* hpng = exp->addAction("History chart (PNG)…", explore_, &ExplorePage::exportChartPng);
    pickActions_ << hcsv << hpng;
    for (auto* a : pickActions_) {
        a->setEnabled(false);
        a->setStatusTip("Click on the model first to choose a point.");
    }
    file->addSeparator();
    file->addAction("E&xit", this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    auto* view = menuBar()->addMenu("&View");
    auto* themes = view->addMenu("&Theme");
    auto* group = new QActionGroup(this);
    const QStringList tn = {"Follow system", "Light", "Dark"};
    const int cur = QSettings().value("theme", 0).toInt();
    for (int i = 0; i < 3; ++i) {
        auto* a = themes->addAction(tn[i], this, [this, i]() { setTheme(i); });
        a->setCheckable(true);
        a->setChecked(i == cur);
        group->addAction(a);
    }
    auto* run = menuBar()->addMenu("&Run");
    runAct_ = run->addAction(style()->standardIcon(QStyle::SP_MediaPlay), "&Start run", this, [this]() {
        goTo(PRun);
        startRun();
    });
    runAct_->setShortcut(Qt::Key_F5);
    cancelAct_ = run->addAction(style()->standardIcon(QStyle::SP_BrowserStop), "&Cancel run", this, [this]() {
        if (!controller_->running()) return;
        if (QMessageBox::question(this, "Stop the run?", "Stop the run? Results computed so far are kept.") == QMessageBox::Yes)
            controller_->cancelRun();
    });
    cancelAct_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    cancelAct_->setEnabled(false);
    auto* help = menuBar()->addMenu("&Help");
    help->addAction("What is verified?", this, &MainWindow::showVerified)->setShortcut(Qt::Key_F1);
    help->addAction("About ABSS", this, [this]() {
        QMessageBox::about(this, "About ABSS",
                           "<b>Abaqus Black-Box Sensitivity Studio</b> 0.1.0<br><br>Parameter sensitivities of Abaqus/Standard "
                           "models at every accepted increment, using Abaqus as an opaque residual/tangent oracle.<br><br>"
                           "Compute backend: " + qs(par::backendDescription()).toHtmlEscaped() +
                               "<br>Qt " + QString(qVersion()) + " · VTK 9<br><br>All processing is local.");
    });
}

void MainWindow::showVerified() {
    QMessageBox box(this);
    box.setWindowTitle("What is verified");
    box.setTextFormat(Qt::RichText);
    box.setText(
        "<b>Status words used in this app</b><ul>"
        "<li><b>Validated</b> — in this run, the derivative agreed with central differences of complete re-analyses at every "
        "increment within tolerance (orders 1–2 only).</li>"
        "<li><b>Experimental</b> — computed and passed the same comparison, but order 3 or higher is not yet considered "
        "established. Use with care.</li>"
        "<li><b>Unverified</b> — computed; not compared with re-analyses (validation switched off).</li>"
        "<li><b>Failed</b> — did not agree with re-analyses within tolerance.</li>"
        "<li><b>Synthetic</b> — demo results from bundled analytic models, not Abaqus.</li></ul>"
        "<b>In this build</b><ul><li>The demo models run the complete sensitivity engine without Abaqus.</li>"
        "<li>Input files can be opened, meshed and searched for parameters. Running them needs the Abaqus residual "
        "oracle; when it is not available in this build the Run page says so.</li>"
        "<li>Increments flagged as possibly non-smooth (yield onset, load reversal) are listed next to the derivative "
        "selector; derivatives there may not exist.</li></ul>");
    box.exec();
}

void MainWindow::setTheme(int mode) {
    QSettings().setValue("theme", mode);
    applyTheme(*qApp, static_cast<ThemeMode>(mode));
    explore_->applyTheme();
    params_->applyTheme();
    updateRail();
}

void MainWindow::goTo(int page) {
    if (page < 0 || page > PResults) return;
    if (auto* it = rail_->item(page); it && !(it->flags() & Qt::ItemIsEnabled)) return;
    stack_->setCurrentIndex(page);
}

void MainWindow::updateRail() {
    const bool model = controller_->model() != nullptr;
    const bool computable = model && controller_->source() != Controller::Source::Project;
    const bool results = controller_->results() != nullptr;
    auto set = [this](int i, bool on, const QString& why) {
        auto* it = rail_->item(i);
        it->setFlags(on ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::NoItemFlags);
        it->setToolTip(on ? QString() : why);
    };
    set(PStart, true, {});
    set(PParams, model, "Open an input file first.");
    set(PSetup, computable, model ? "These results were opened from a file; open its input file or a demo to set up a new run."
                                  : "Open an input file first.");
    set(PRun, computable, "Finish the setup first: open an input file or a demo.");
    set(PResults, results, "No results yet. Run an analysis or try the demo.");
    for (auto* a : resultActions_) a->setEnabled(results);
    for (auto* a : pickActions_) a->setEnabled(results && explore_->hasPicks());
}

void MainWindow::updateTitle() {
    QString t = "ABSS";
    const QString n = controller_->displayName();
    if (!n.isEmpty()) t += " — " + n;
    auto m = controller_->results() ? controller_->results() : controller_->model();
    if (m && m->synthetic) t += " — DEMO (synthetic)";
    setWindowTitle(t);
}

void MainWindow::updateRunEnable() {
    QString reason = setup_->firstProblem();
    if (reason.isEmpty() && controller_->source() == Controller::Source::Inp && abaqus_ && !abaqus_->found)
        reason = "Abaqus is needed to run new analyses. Locate it on the Start page.";
    if (reason.isEmpty() && controller_->running()) reason = "A run is already in progress.";
    run_->setStartEnabled(reason.isEmpty(), reason);
    runAct_->setEnabled(reason.isEmpty());
    const auto s = setup_->settings();
    const auto c = controller_->estimate(s);
    QString sum = controller_->displayName();
    if (!sum.isEmpty()) sum += " · ";
    sum += c.lines.join(" · ");
    run_->setSummary(sum);
}

void MainWindow::detectAbaqusAsync() {
    if (detecting_) return;
    detecting_ = true;
    start_->setAbaqusChecking();
    abqStatus_->setText("Abaqus: checking…");
    const std::string override = QSettings().value("abaqusCommand").toString().toStdString();
    auto result = std::make_shared<AbaqusInstallation>();
    QThread* t = QThread::create([result, override]() {
        try {
            *result = detectAbaqus(override);
        } catch (const std::exception& e) {
            result->found = false;
            result->message = e.what();
        }
    });
    connect(t, &QThread::finished, this, [this, t, result]() {
        detecting_ = false;
        abaqus_ = *result;
        start_->setAbaqus(*result);
        abqStatus_->setText(result->found ? "Abaqus: found" + (result->version.empty() ? QString() : " (" + qs(result->version) + ")")
                                          : "Abaqus: not found");
        updateRunEnable();
        t->deleteLater();
    });
    t->start();
}

void MainWindow::chooseFile() {
    const QString dir = QSettings().value("lastDir", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString p = QFileDialog::getOpenFileName(this, "Open an input file or results", dir,
                                                   "Abaqus input or ABSS results (*.inp *.abss);;Abaqus input (*.inp);;ABSS results (*.abss)");
    if (!p.isEmpty()) openPath(p);
}

void MainWindow::openPath(const QString& path) {
    QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
    const QString ext = QFileInfo(path).suffix().toLower();
    StartPage::addRecent(QFileInfo(path).absoluteFilePath());
    start_->refreshRecent();
    if (ext == "abss") controller_->loadProjectAsync(path);
    else controller_->loadInpAsync(path);
}

void MainWindow::startRun() {
    if (controller_->running()) return;
    QString reason = setup_->firstProblem();
    if (reason.isEmpty() && controller_->source() == Controller::Source::Inp && abaqus_ && !abaqus_->found)
        reason = "Abaqus is needed to run new analyses. Locate it on the Start page.";
    if (!reason.isEmpty()) {
        readout_->setText(reason);
        return;
    }
    controller_->startRun(setup_->settings());
}

void MainWindow::saveProject() {
    if (!controller_->results()) return;
    if (controller_->projectPath().isEmpty()) return saveProjectAs();
    QString err;
    if (!controller_->saveProject(controller_->projectPath(), &err))
        QMessageBox::warning(this, "The results could not be saved", err);
    else
        readout_->setText("Saved " + QDir::toNativeSeparators(controller_->projectPath()));
}

void MainWindow::saveProjectAs() {
    if (!controller_->results()) return;
    QString base = QString::fromStdString(controller_->results()->title);
    base.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
    const QString dir = QSettings().value("lastDir", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString p = QFileDialog::getSaveFileName(this, "Save results", QDir(dir).filePath(base + ".abss"), "ABSS results (*.abss)");
    if (p.isEmpty()) return;
    QString err;
    if (!controller_->saveProject(p, &err)) {
        QMessageBox::warning(this, "The results could not be saved", err);
        return;
    }
    StartPage::addRecent(p);
    start_->refreshRecent();
    readout_->setText("Saved " + QDir::toNativeSeparators(p));
    updateTitle();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (controller_->running()) {
        if (!auto_ && QMessageBox::question(this, "A run is in progress", "Stop the run and close ABSS?") != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        controller_->cancelRun();
    }
    e->accept();
}

void MainWindow::startAutomation(const Automation& a) {
    auto_ = a;
    if (!a.demoKey.isEmpty()) {
        const auto m = demoFromKey(a.demoKey);
        if (!m) {
            QMessageBox::warning(this, "Unknown demo", "Unknown demo \"" + a.demoKey + "\". Known: plate, cantilever, truss, truss-plastic, quad.");
            return;
        }
        pendingRun_ = a.run;
        controller_->loadDemoAsync(*m);
    } else if (!a.openPath.isEmpty()) {
        pendingRun_ = a.run && !a.openPath.endsWith(".abss", Qt::CaseInsensitive);
        openPath(a.openPath);
    } else if (!a.screenshot.isEmpty() || a.quit) {
        // Nothing to load: wait for the Abaqus readiness check so the Start page shows its final state.
        auto* timer = new QTimer(this);
        auto waited = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [this, timer, waited]() {
            *waited += 250;
            if (!detecting_ || *waited > 15000) {
                timer->stop();
                timer->deleteLater();
                QTimer::singleShot(400, this, &MainWindow::finishAutomation);
            }
        });
        timer->start(250);
    }
}

void MainWindow::finishAutomation() {
    if (!auto_) return;
    const Automation a = *auto_;
    auto_.reset();
    if (controller_->results() && explore_->hasStore()) {
        if (a.resultOrder >= 0) {
            const auto& sp = explore_->spec();
            int param = sp.derivative.params.empty() ? -1 : sp.derivative.params.front();
            if (param < 0)
                for (const auto& d : controller_->results()->derivatives())
                    if (!d.params.empty()) {
                        param = d.params.front();
                        break;
                    }
            explore_->selectDerivative(param, a.resultOrder);
        }
        if (!a.output.isEmpty()) explore_->selectOutput(a.output);
        if (a.frame >= 0) explore_->setFrame(a.frame);
        if (a.picks > 0) explore_->autoPick(a.picks);
    }
    if (!a.saveProject.isEmpty() && controller_->results()) {
        QString err;
        if (!controller_->saveProject(a.saveProject, &err)) qWarning("save failed: %s", qPrintable(err));
    }
    if (!a.page.isEmpty()) {
        const QStringList pages = {"start", "parameters", "setup", "run", "results"};
        const int i = pages.indexOf(a.page.toLower());
        if (i >= 0) goTo(i);
    }
    QTimer::singleShot(700, this, [this, a]() {
        if (!a.viewShot.isEmpty()) explore_->viewImage(true).save(a.viewShot);
        if (!a.chartShot.isEmpty()) explore_->chartImage().save(a.chartShot);
        if (!a.screenshot.isEmpty()) {
            const QPixmap pm = grab();
            if (!pm.save(a.screenshot)) qWarning("Could not save %s", qPrintable(a.screenshot));
        }
        if (a.quit || !a.screenshot.isEmpty()) QTimer::singleShot(100, qApp, &QApplication::quit);
    });
}

}  // namespace abss::gui
