// ABSS GUI: workflow pages Start, Parameters, Set up and Run (Results is ExplorePage).
#pragma once

#include "abss/gui/controller.hpp"
#include "abss/oracle/abaqus.hpp"

#include <QFrame>
#include <QWidget>

#include <vector>

class QLineEdit;
class QSpinBox;
class QLabel;

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace abss::gui {

class VtkView;

/// Error panel per UX rule 6: title, cause, "What you can do", expandable details.
class ErrorPanel : public QFrame {
    Q_OBJECT
public:
    explicit ErrorPanel(QWidget* parent = nullptr);
    void showError(const QString& title, const QString& cause, const QStringList& actions, const QString& details);
private:
    QLabel* title_;
    QLabel* cause_;
    QLabel* actions_;
    QPlainTextEdit* details_;
    QToolButton* toggle_;
};

class StartPage : public QWidget {
    Q_OBJECT
public:
    explicit StartPage(Controller* c, QWidget* parent = nullptr);
    void setAbaqusChecking();
    void setAbaqus(const AbaqusInstallation& inst);
    void setLicenseChecking();
    void setLicenseResult(const AbaqusInstallation& inst);
    void refreshRecent();
    void showLoadError(const QString& title, const QString& detail);
    void setBusy(bool busy, const QString& what);
    static void addRecent(const QString& path);
    static QStringList recentFiles();

signals:
    void openFileRequested(const QString& path);
    void chooseFileRequested();
    void demoRequested(DemoModel m, bool runNow);
    void locateAbaqusRequested();
    void licenseCheckRequested();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    Controller* controller_;
    QFrame* drop_;
    QLabel* dropMsg_;
    QComboBox* demo_;
    QLabel* demoDesc_;
    QListWidget* recent_;
    QLabel* abqWord_;
    QLabel* abqDetail_;
    QLabel* abqDot_;
    QPushButton* abqBtn_;
    QLabel* licWord_;
    QLabel* licDetail_;
    QLabel* licDot_;
    QPushButton* licBtn_;
    QLabel* busy_;
    ErrorPanel* error_;
    bool abaqusFound_ = false;
};

class ParamsPage : public QWidget {
    Q_OBJECT
public:
    explicit ParamsPage(Controller* c, QWidget* parent = nullptr);
    void reload();
    std::vector<int> selected() const;
    void setSelected(const std::vector<int>& idx);
    void setLocked(bool locked);
    void focusSearch();
    void applyTheme();

signals:
    void selectionChanged();
    void nextRequested();

private:
    void applyFilter();
    void showInfo(int row);
    void updateCounter();

    Controller* controller_;
    QLineEdit* search_;
    std::vector<QToolButton*> chips_;
    QCheckBox* showUnsupported_;
    QLabel* counter_;
    QTableWidget* table_;
    QLabel* empty_;
    QLabel* info_;
    QLabel* snippet_;
    QLabel* summary_;
    QLabel* diag_;
    VtkView* preview_;
    QPushButton* next_;
    QFrame* lockBanner_;
    bool loading_ = false;
};

class SetupPage : public QWidget {
    Q_OBJECT
public:
    explicit SetupPage(Controller* c, QWidget* parent = nullptr);
    void reload();
    void setParameters(const std::vector<int>& idx);
    RunSettings settings() const;
    void setOrder(int k);
    void setLocked(bool locked);
    QString firstProblem() const;  ///< empty when the setup is valid

signals:
    void settingsChanged();
    void nextRequested();

private:
    void updateCost();

    Controller* controller_;
    std::vector<int> params_;
    QButtonGroup* order_;
    QLabel* orderNote_;
    QLabel* expChip_;
    std::vector<QCheckBox*> outputs_;
    QLabel* outputsNote_;
    QCheckBox* validate_;
    QWidget* abaqusBox_ = nullptr;
    QLineEdit* userSub_ = nullptr;
    QSpinBox* jobs_ = nullptr;
    QLabel* userSubNote_ = nullptr;
    QCheckBox* tangent_;
    QDoubleSpinBox* step_;
    QWidget* advanced_;
    QLabel* cost_;
    QLabel* warnings_;
    QLabel* paramsSummary_;
    QPushButton* next_;
    QFrame* lockBanner_;
    bool experimentalConfirmed_ = false;
};

class RunPage : public QWidget {
    Q_OBJECT
public:
    explicit RunPage(Controller* c, QWidget* parent = nullptr);
    void setStartEnabled(bool on, const QString& reason);
    void onStarted();
    void onProgress(const QString& stage, double fraction);
    void onLog(const QString& line);
    void onFinished(bool ok, bool cancelled, const QString& message, const QString& detail);
    void setSummary(const QString& text);

signals:
    void startRequested();
    void cancelRequested();
    void openResultsRequested();

private:
    Controller* controller_;
    QLabel* status_;
    QLabel* summary_;
    QPushButton* start_;
    QPushButton* cancel_;
    QProgressBar* bar_;
    QLabel* stage_;
    QToolButton* logToggle_;
    QPlainTextEdit* log_;
    QPushButton* copyLog_;
    QPushButton* openFolder_;
    ErrorPanel* error_;
    QFrame* done_;
    QString logPath_;
};

}  // namespace abss::gui
