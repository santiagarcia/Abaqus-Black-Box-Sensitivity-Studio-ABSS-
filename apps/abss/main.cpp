// ABSS desktop application entry point.
//
//   abss                                   start on the Welcome page
//   abss model.inp | results.abss          open a file
//   abss --demo plate --run                run a bundled demo and open the results
//   abss --demo plate --run --pick 2 --screenshot docs/screenshots/explore.png
//        (unattended: load, run, pick points, save a PNG of the window, quit)
#include "abss/gui/main_window.hpp"
#include "abss/gui/theme.hpp"
#include "abss/linalg/parallel.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>

#include <exception>
#include <iostream>

namespace {
void crashHandler() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir().mkpath(dir);
    QFile f(dir + "/crash-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt");
    if (f.open(QIODevice::WriteOnly)) {
        QString what = "unknown";
        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = QString::fromUtf8(e.what());
            } catch (...) {
            }
        }
        f.write(("ABSS terminated unexpectedly: " + what + "\n").toUtf8());
    }
    std::abort();
}
}  // namespace

int main(int argc, char** argv) {
    std::set_terminate(crashHandler);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    QApplication::setOrganizationName("ABSS");
    QApplication::setApplicationName("ABSS");
    QApplication::setApplicationVersion("0.1.0");
    abss::par::initialize();

    QCommandLineParser p;
    p.setApplicationDescription("Abaqus Black-Box Sensitivity Studio");
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption demo("demo", "Open a bundled demo: plate, cantilever, truss, truss-plastic, quad.", "key");
    QCommandLineOption run("run", "Run with the default setup after loading.");
    QCommandLineOption order("order", "Derivative order for --run (0-3).", "k");
    QCommandLineOption params("params", "Comma-separated parameter names to select (e.g. E,CF1).", "names");
    QCommandLineOption output("output", "Output to display on the Results page, e.g. U2, S_Mises.", "quantity");
    QCommandLineOption resultOrder("result-order", "Derivative order to display on the Results page.", "k");
    QCommandLineOption frame("frame", "Increment to display.", "i");
    QCommandLineOption pick("pick", "Pick this many points for the history chart.", "n");
    QCommandLineOption page("page", "Page to show: start, parameters, setup, run, results.", "name");
    QCommandLineOption shot("screenshot", "Save a PNG of the window, then quit.", "png");
    QCommandLineOption viewShot("view-shot", "Also save the 3D view as PNG.", "png");
    QCommandLineOption chartShot("chart-shot", "Also save the history chart as PNG.", "png");
    QCommandLineOption save("save", "Save the results to this .abss file after the run.", "abss");
    QCommandLineOption theme("theme", "Theme: system, light, dark.", "name");
    QCommandLineOption size("size", "Window size WxH.", "WxH");
    QCommandLineOption quit("quit", "Quit when the automation is finished.");
    for (const auto& o : {demo, run, order, params, output, resultOrder, frame, pick, page, shot, viewShot, chartShot, save, theme, size, quit})
        p.addOption(o);
    p.addPositionalArgument("file", "Abaqus input (.inp) or ABSS results (.abss) to open.", "[file]");
    p.process(app);

    int themeMode = QSettings().value("theme", 0).toInt();
    if (p.isSet(theme)) {
        const QString t = p.value(theme).toLower();
        themeMode = t == "light" ? 1 : t == "dark" ? 2 : 0;
    }
    abss::gui::applyTheme(app, static_cast<abss::gui::ThemeMode>(themeMode));
    QApplication::setWindowIcon(abss::gui::appIcon());

    abss::gui::MainWindow w;
    if (p.isSet(size)) {
        const auto parts = p.value(size).toLower().split('x');
        if (parts.size() == 2) w.resize(parts[0].toInt(), parts[1].toInt());
    }
    w.show();

    abss::gui::Automation a;
    a.demoKey = p.value(demo);
    if (!p.positionalArguments().isEmpty()) a.openPath = p.positionalArguments().front();
    a.run = p.isSet(run);
    a.order = p.isSet(order) ? p.value(order).toInt() : -1;
    if (p.isSet(params)) a.params = p.value(params).split(',', Qt::SkipEmptyParts);
    a.output = p.value(output);
    a.resultOrder = p.isSet(resultOrder) ? p.value(resultOrder).toInt() : -1;
    a.frame = p.isSet(frame) ? p.value(frame).toInt() : -1;
    a.picks = p.isSet(pick) ? p.value(pick).toInt() : 0;
    a.page = p.value(page);
    a.screenshot = p.value(shot);
    a.viewShot = p.value(viewShot);
    a.chartShot = p.value(chartShot);
    a.saveProject = p.value(save);
    a.quit = p.isSet(quit);
    if (!a.demoKey.isEmpty() || !a.openPath.isEmpty() || !a.screenshot.isEmpty() || a.quit) w.startAutomation(a);
    return app.exec();
}
