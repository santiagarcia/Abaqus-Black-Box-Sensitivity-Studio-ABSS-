// ABSS GUI: the Results (Explore) page — what-to-show bar, 3D view, display controls, timeline, history chart.
#pragma once

#include "abss/core/results.hpp"
#include "abss/gui/controller.hpp"
#include "abss/viz/display.hpp"

#include <QMainWindow>
#include <QTimer>
#include <QWidget>

#include <map>
#include <memory>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QToolButton;
class QTreeWidget;

namespace abss::gui {

class VtkView;
class HistoryChart;

class ExplorePage : public QWidget {
    Q_OBJECT
public:
    ExplorePage(Controller* controller, QWidget* parent = nullptr);

    /// Show a result store (resets selection to defaults, keeps camera unless the mesh changed).
    void setStore(std::shared_ptr<const ResultStore> store);
    bool hasStore() const { return static_cast<bool>(store_); }
    const viz::DisplaySpec& spec() const { return spec_; }
    int frame() const { return frame_; }
    void setFrame(int f);
    /// Programmatic selection (command line / tests).
    void selectOutput(const QString& quantity);
    void selectDerivative(int param, int order);
    /// Pick up to n nodes/elements with the largest |value| at the current increment (screenshots, demos).
    void autoPick(int n);
    void setStaleBanner(bool on);
    void applyTheme();

    QImage viewImage(bool withFooter);
    QImage chartImage();
    QString currentHistoryCsv() const;
    bool hasPicks() const { return !picks_.empty(); }

public slots:
    void exportHistoryCsv();
    void copyHistory();
    void exportChartPng();
    void exportScreenshot();
    void exportFrameCsv();
    void exportVtu();

signals:
    void statusMessage(const QString& text);
    void exportsChanged();

private:
    struct PickRec {
        std::int32_t cell = -1;
        std::int32_t node = -1;
    };

    QWidget* buildWhatBar();
    QWidget* buildDisplayDock();
    QWidget* buildTimeline();
    QWidget* buildHistoryPanel();
    QWidget* buildViewToolbar();

    void rebuildSelectors();
    void rebuildOutputCombo();
    void rebuildWhereCombo();
    void updateOrderButtons();
    void updateQuality();
    void updateModelTree();
    DerivativeSpec currentDerivative() const;
    viz::PickTarget targetOf(const PickRec& p) const;
    void onSelectionChanged();
    void applyDefaultsForSelection();
    void refreshView(bool rangeChanged);
    void refreshChart();
    void refreshMarkers();
    void refreshTimelineLabel();
    void onPicked(int cell, int node, bool add);
    void onKeyAction(const QString& a);
    void stepFrame(int delta);
    void togglePlay();
    QString colormapKey() const;
    QString footerText() const;
    QString saveName(const QString& suffix) const;

    Controller* controller_;
    std::shared_ptr<const ResultStore> store_;
    viz::DisplaySpec spec_;
    int frame_ = 0;
    int param_ = -1;
    int order_ = 0;
    std::vector<PickRec> picks_;
    std::map<QString, QString> userColormap_;  // per (output, order) for the session
    viz::Range allRange_;
    bool allRangeValid_ = false;
    bool updating_ = false;
    bool firstMesh_ = true;
    QTimer play_;

    // what-to-show bar
    QFrame* demoBanner_ = nullptr;
    QFrame* staleBanner_ = nullptr;
    QComboBox* output_ = nullptr;
    QComboBox* where_ = nullptr;
    QComboBox* param_combo_ = nullptr;
    QButtonGroup* orderGroup_ = nullptr;
    std::vector<QToolButton*> orderButtons_;
    QLabel* qualityBadge_ = nullptr;
    QLabel* syntheticBadge_ = nullptr;
    QToolButton* evidenceBtn_ = nullptr;
    QLabel* suspect_ = nullptr;
    QLabel* question_ = nullptr;
    QLabel* projection_ = nullptr;

    // view
    QMainWindow* inner_ = nullptr;
    VtkView* view_ = nullptr;
    QToolButton* orthoBtn_ = nullptr;

    // display dock
    QCheckBox* deformed_ = nullptr;
    QDoubleSpinBox* scale_ = nullptr;
    QPushButton* autoScale_ = nullptr;
    QCheckBox* undeformed_ = nullptr;
    QCheckBox* edges_ = nullptr;
    QCheckBox* wire_ = nullptr;
    QSlider* opacity_ = nullptr;
    QComboBox* cmap_ = nullptr;
    QRadioButton* rangeAll_ = nullptr;
    QRadioButton* rangeCur_ = nullptr;
    QRadioButton* rangeMan_ = nullptr;
    QLineEdit* min_ = nullptr;
    QLineEdit* max_ = nullptr;
    QCheckBox* centered_ = nullptr;
    QComboBox* scaleMode_ = nullptr;
    QLabel* rangeInfo_ = nullptr;

    // timeline
    QWidget* timeline_ = nullptr;
    QToolButton* first_ = nullptr;
    QToolButton* prev_ = nullptr;
    QToolButton* playBtn_ = nullptr;
    QToolButton* next_ = nullptr;
    QToolButton* last_ = nullptr;
    QComboBox* speed_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* frameLabel_ = nullptr;

    // history
    HistoryChart* chart_ = nullptr;
    QComboBox* axis_ = nullptr;
    QCheckBox* nominal_ = nullptr;
    QPushButton* clear_ = nullptr;
    QPushButton* csv_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* png_ = nullptr;

    QTreeWidget* tree_ = nullptr;
};

}  // namespace abss::gui
