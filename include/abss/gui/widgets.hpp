// ABSS GUI: reusable widgets — the VTK 3D view and the history chart.
#pragma once

#include "abss/viz/scene.hpp"

#include <QChartView>
#include <QImage>
#include <QPoint>
#include <QVTKOpenGLNativeWidget.h>

#include <memory>
#include <vector>

class QLabel;
class QLineSeries;
class QValueAxis;
class vtkOrientationMarkerWidget;

namespace abss::gui {

/// QVTKOpenGLNativeWidget hosting a viz::Scene. Click (< 4 px movement) picks; drag orbits; right-drag pans;
/// wheel zooms. VTK's own hotkeys are disabled; ABSS keys are reported through keyAction().
class VtkView : public QVTKOpenGLNativeWidget {
    Q_OBJECT
public:
    explicit VtkView(QWidget* parent = nullptr);
    ~VtkView() override;
    viz::Scene& scene() { return *scene_; }
    void render();
    QImage grabImage();
    void setAxesVisible(bool on);

signals:
    void picked(int cell, int node, bool add);
    void keyAction(const QString& action);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    std::unique_ptr<viz::Scene> scene_;
    vtkOrientationMarkerWidget* axes_ = nullptr;
    QPoint pressPos_;
    bool leftDown_ = false;
};

/// History chart (QtCharts): one or more value-vs-time series, an optional secondary axis, a current-increment
/// cursor, hover read-out and click-to-seek.
class HistoryChart : public QChartView {
    Q_OBJECT
public:
    struct Series {
        QString name;
        std::vector<double> y;
        QColor color;
        bool dashed = false;
        bool secondary = false;
    };
    explicit HistoryChart(QWidget* parent = nullptr);
    void setData(const QString& title, const QString& xTitle, const QString& yTitle, const QString& y2Title,
                 const std::vector<double>& x, const std::vector<Series>& series);
    void setCursorX(double x);
    void showEmpty(const QString& text);
    bool hasData() const { return hasData_; }
    void applyTheme();
    /// Image of the chart with an optional footer line (e.g. the synthetic-data marker).
    QImage renderImage(const QString& footer);

signals:
    void xClicked(double x);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void placeEmptyLabel();
    QLabel* empty_ = nullptr;
    QLineSeries* cursor_ = nullptr;
    QValueAxis* ax_ = nullptr;
    QValueAxis* ay_ = nullptr;
    QValueAxis* ay2_ = nullptr;
    bool hasData_ = false;
    double cursorX_ = 0;
    int themeApplied_ = -1;
};

}  // namespace abss::gui
