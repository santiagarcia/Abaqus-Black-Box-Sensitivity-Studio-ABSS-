#include "abss/gui/widgets.hpp"

#include "abss/gui/theme.hpp"

#include <QChart>
#include <QKeyEvent>
#include <QLabel>
#include <QLegendMarker>
#include <QLineSeries>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QValueAxis>

#include <vtkAxesActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCaptionActor2D.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkTextProperty.h>

#include <cmath>

namespace abss::gui {

namespace {
/// Trackball camera without VTK's single-key commands (which would e.g. quit or switch to stereo), right-drag pans.
class AbssStyle : public vtkInteractorStyleTrackballCamera {
public:
    static AbssStyle* New();
    vtkTypeMacro(AbssStyle, vtkInteractorStyleTrackballCamera);
    void OnChar() override {}
    void OnKeyPress() override {}
    void OnKeyRelease() override {}
    void OnRightButtonDown() override {
        if (!this->Interactor) return;
        this->FindPokedRenderer(this->Interactor->GetEventPosition()[0], this->Interactor->GetEventPosition()[1]);
        if (!this->CurrentRenderer) return;
        this->GrabFocus(this->EventCallbackCommand);
        this->StartPan();
    }
    void OnRightButtonUp() override {
        if (this->State == VTKIS_PAN) this->EndPan();
        if (this->Interactor) this->ReleaseFocus();
    }
};
vtkStandardNewMacro(AbssStyle);
}  // namespace

VtkView::VtkView(QWidget* parent) : QVTKOpenGLNativeWidget(parent), scene_(std::make_unique<viz::Scene>()) {
    vtkNew<vtkGenericOpenGLRenderWindow> win;
    win->SetMultiSamples(4);
    setRenderWindow(win);
    win->AddRenderer(scene_->renderer());
    vtkNew<AbssStyle> style;
    interactor()->SetInteractorStyle(style);
    vtkNew<vtkAxesActor> a;
    a->SetShaftTypeToCylinder();
    a->SetCylinderRadius(0.03);
    a->SetTotalLength(1, 1, 1);
    const double col[3][3] = {{0.85, 0.25, 0.25}, {0.2, 0.65, 0.3}, {0.25, 0.45, 0.9}};
    vtkCaptionActor2D* caps[3] = {a->GetXAxisCaptionActor2D(), a->GetYAxisCaptionActor2D(), a->GetZAxisCaptionActor2D()};
    for (int i = 0; i < 3; ++i) {
        auto* tp = caps[i]->GetCaptionTextProperty();
        tp->SetColor(col[i][0], col[i][1], col[i][2]);
        tp->ShadowOff();
        tp->ItalicOff();
        tp->BoldOn();
    }
    axes_ = vtkOrientationMarkerWidget::New();
    axes_->SetOrientationMarker(a);
    axes_->SetInteractor(interactor());
    axes_->SetViewport(0.88, 0.0, 1.0, 0.18);
    axes_->SetEnabled(1);
    axes_->InteractiveOff();
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
}

VtkView::~VtkView() {
    if (axes_) {
        axes_->SetEnabled(0);
        axes_->Delete();
    }
}

void VtkView::render() {
    if (renderWindow()) renderWindow()->Render();
    update();
}

QImage VtkView::grabImage() {
    render();
    return grabFramebuffer();
}

void VtkView::setAxesVisible(bool on) { axes_->SetEnabled(on ? 1 : 0); }

void VtkView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        pressPos_ = e->position().toPoint();
        leftDown_ = true;
    }
    setFocus(Qt::MouseFocusReason);
    QVTKOpenGLNativeWidget::mousePressEvent(e);
}

void VtkView::mouseReleaseEvent(QMouseEvent* e) {
    QVTKOpenGLNativeWidget::mouseReleaseEvent(e);
    if (e->button() != Qt::LeftButton || !leftDown_) return;
    leftDown_ = false;
    const QPoint p = e->position().toPoint();
    if ((p - pressPos_).manhattanLength() > 4) return;  // a drag, not a click
    const double dpr = devicePixelRatioF();
    const int x = static_cast<int>(std::lround(p.x() * dpr));
    const int y = static_cast<int>(std::lround((height() - p.y() - 1) * dpr));
    const auto pk = scene_->pick(x, y);
    if (pk.hit) emit picked(pk.cell, pk.node, (e->modifiers() & Qt::ControlModifier) != 0);
}

void VtkView::keyPressEvent(QKeyEvent* e) {
    QString a;
    switch (e->key()) {
        case Qt::Key_F: a = "fit"; break;
        case Qt::Key_I: a = "iso"; break;
        case Qt::Key_1: a = "xy"; break;
        case Qt::Key_2: a = "xz"; break;
        case Qt::Key_3: a = "yz"; break;
        case Qt::Key_P: a = "projection"; break;
        case Qt::Key_E: a = "edges"; break;
        case Qt::Key_D: a = "deformed"; break;
        case Qt::Key_Space: a = "play"; break;
        case Qt::Key_Left: a = "prev"; break;
        case Qt::Key_Right: a = "next"; break;
        case Qt::Key_Home: a = "first"; break;
        case Qt::Key_End: a = "last"; break;
        case Qt::Key_Escape: a = "clearpicks"; break;
        default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
    emit keyAction(a);
}

// ---- history chart ---------------------------------------------------------------------------------------------

HistoryChart::HistoryChart(QWidget* parent) : QChartView(parent) {
    auto* c = new QChart();
    c->legend()->setAlignment(Qt::AlignBottom);
    c->setMargins(QMargins(6, 6, 6, 6));
    c->setBackgroundRoundness(0);
    setChart(c);
    setRenderHint(QPainter::Antialiasing);
    empty_ = new QLabel(this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setProperty("secondary", true);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents);
    applyTheme();
    showEmpty("Click on the model to plot the history at that point. Ctrl+click adds more points.");
}

void HistoryChart::applyTheme() {
    const auto& t = tokens();
    auto* c = chart();
    const int theme = t.dark ? 1 : 0;
    if (theme != themeApplied_ && c->series().isEmpty()) {
        c->setTheme(t.dark ? QChart::ChartThemeDark : QChart::ChartThemeLight);
        themeApplied_ = theme;
    }
    c->setBackgroundBrush(t.panel);
    c->setBackgroundPen(Qt::NoPen);
    c->setPlotAreaBackgroundVisible(false);
    c->setTitleBrush(t.text);
    c->legend()->setLabelColor(t.text);
    QFont tf = font();
    tf.setPointSizeF(9.5);
    tf.setBold(true);
    c->setTitleFont(tf);
    QFont lf = font();
    lf.setPointSizeF(8.5);
    c->legend()->setFont(lf);
    for (auto* a : c->axes()) {
        a->setLabelsFont(lf);
        a->setTitleFont(lf);
        a->setLabelsColor(t.text2);
        a->setTitleBrush(t.text2);
        a->setGridLineColor(t.border);
        a->setLinePenColor(t.border);
    }
    empty_->setStyleSheet(QString("color: %1; font-size: 10pt;").arg(t.text2.name()));
}

void HistoryChart::placeEmptyLabel() { empty_->setGeometry(rect().adjusted(24, 24, -24, -24)); }

void HistoryChart::resizeEvent(QResizeEvent* e) {
    QChartView::resizeEvent(e);
    placeEmptyLabel();
}

void HistoryChart::showEmpty(const QString& text) {
    auto* c = chart();
    c->removeAllSeries();
    for (auto* a : c->axes()) {
        c->removeAxis(a);
        delete a;
    }
    cursor_ = nullptr;
    ax_ = ay_ = ay2_ = nullptr;
    c->setTitle(QString());
    hasData_ = false;
    empty_->setText(text);
    empty_->show();
    placeEmptyLabel();
    applyTheme();
}

void HistoryChart::setData(const QString& title, const QString& xTitle, const QString& yTitle, const QString& y2Title,
                           const std::vector<double>& x, const std::vector<Series>& series) {
    showEmpty({});
    empty_->hide();
    auto* c = chart();
    c->setTitle(title);
    ax_ = new QValueAxis;
    ax_->setTitleText(xTitle);
    ax_->setLabelFormat("%.4g");
    ay_ = new QValueAxis;
    ay_->setTitleText(yTitle);
    ay_->setLabelFormat("%.3g");
    c->addAxis(ax_, Qt::AlignBottom);
    c->addAxis(ay_, Qt::AlignLeft);
    bool anySecondary = false;
    for (const auto& s : series) anySecondary = anySecondary || s.secondary;
    if (anySecondary) {
        ay2_ = new QValueAxis;
        ay2_->setTitleText(y2Title);
        ay2_->setLabelFormat("%.3g");
        c->addAxis(ay2_, Qt::AlignRight);
    }
    // Very small/large magnitudes (sensitivities are often ~1e-8 .. 1e-17) are plotted in scaled units so the axis
    // arithmetic stays well conditioned; the axis title states the factor and tooltips show true values.
    double m1 = 0, m2 = 0;
    for (const auto& s : series)
        for (double v : s.y)
            if (v == v && !std::isinf(v)) (s.secondary ? m2 : m1) = std::max(s.secondary ? m2 : m1, std::abs(v));
    auto exponentFor = [](double m) { return (m > 0 && (m < 1e-3 || m >= 1e5)) ? static_cast<int>(std::floor(std::log10(m))) : 0; };
    const int e1 = exponentFor(m1), e2 = exponentFor(m2);
    const double f1 = std::pow(10.0, -e1), f2 = std::pow(10.0, -e2);
    if (e1 != 0) ay_->setTitleText(QString("×1e%1").arg(e1));
    if (ay2_ && e2 != 0) ay2_->setTitleText(y2Title + QString(" ×1e%1").arg(e2));
    double xmin = 1e300, xmax = -1e300, y1min = 1e300, y1max = -1e300, y2min = 1e300, y2max = -1e300;
    for (double v : x) {
        xmin = std::min(xmin, v);
        xmax = std::max(xmax, v);
    }
    for (const auto& s : series) {
        auto* ls = new QLineSeries;
        ls->setName(s.name);
        QPen pen(s.color, s.secondary ? 1.6 : 2.2);
        if (s.dashed) pen.setStyle(Qt::DashLine);
        if (s.secondary) pen.setStyle(Qt::DotLine);
        ls->setPen(pen);
        ls->setPointsVisible(!s.secondary);
        ls->setMarkerSize(6);
        for (size_t i = 0; i < x.size() && i < s.y.size(); ++i) {
            if (!(s.y[i] == s.y[i]) || std::isinf(s.y[i])) continue;
            const double v = s.y[i] * (s.secondary ? f2 : f1);
            ls->append(x[i], v);
            if (s.secondary) {
                y2min = std::min(y2min, v);
                y2max = std::max(y2max, v);
            } else {
                y1min = std::min(y1min, v);
                y1max = std::max(y1max, v);
            }
        }
        c->addSeries(ls);
        ls->attachAxis(ax_);
        ls->attachAxis(s.secondary && ay2_ ? ay2_ : ay_);
        const QString nm = s.name;
        const double unscale = 1.0 / (s.secondary ? f2 : f1);
        connect(ls, &QLineSeries::hovered, this, [nm, xTitle, unscale](const QPointF& p, bool state) {
            if (state)
                QToolTip::showText(QCursor::pos(), QString("%1 = %2\n%3 = %4").arg(xTitle).arg(p.x(), 0, 'g', 6).arg(nm).arg(p.y() * unscale, 0, 'g', 6));
            else
                QToolTip::hideText();
        });
    }
    auto pad = [](double& lo, double& hi) {
        if (lo > hi) {
            lo = 0;
            hi = 1;
            return;
        }
        if (hi == lo) {
            const double d = lo == 0 ? 1.0 : std::abs(lo) * 0.05;
            lo -= d;
            hi += d;
            return;
        }
        const double d = 0.06 * (hi - lo);
        lo -= d;
        hi += d;
    };
    pad(y1min, y1max);
    pad(y2min, y2max);
    if (!(xmax > xmin)) {
        xmin -= 0.5;
        xmax += 0.5;
    }
    ax_->setRange(xmin, xmax);
    ay_->setRange(y1min, y1max);
    if (ay2_) ay2_->setRange(y2min, y2max);
    cursor_ = new QLineSeries;
    cursor_->setName("current increment");
    QPen cp(tokens().text2, 1.2, Qt::DashLine);
    cursor_->setPen(cp);
    c->addSeries(cursor_);
    cursor_->attachAxis(ax_);
    cursor_->attachAxis(ay_);
    for (auto* m : c->legend()->markers(cursor_)) m->setVisible(false);
    hasData_ = true;
    applyTheme();
    setCursorX(cursorX_);
}

void HistoryChart::setCursorX(double x) {
    cursorX_ = x;
    if (!cursor_ || !ay_) return;
    cursor_->clear();
    cursor_->append(x, ay_->min());
    cursor_->append(x, ay_->max());
}

void HistoryChart::mousePressEvent(QMouseEvent* e) {
    if (hasData_ && e->button() == Qt::LeftButton && ax_) {
        const QPointF scenePos = mapToScene(e->position().toPoint());
        const QPointF chartPos = chart()->mapFromScene(scenePos);
        if (chart()->plotArea().contains(chartPos)) {
            const auto ser = chart()->series();
            if (!ser.isEmpty()) emit xClicked(chart()->mapToValue(chartPos, ser.front()).x());
        }
    }
    QChartView::mousePressEvent(e);
}

QImage HistoryChart::renderImage(const QString& footer) {
    const QPixmap pm = grab();
    if (footer.isEmpty()) return pm.toImage();
    const int extra = 26;
    QImage img(pm.width(), pm.height() + static_cast<int>(extra * pm.devicePixelRatio()), QImage::Format_ARGB32);
    img.setDevicePixelRatio(pm.devicePixelRatio());
    img.fill(tokens().panel);
    QPainter p(&img);
    p.drawPixmap(0, 0, pm);
    p.setPen(tokens().bannerText);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    const double h = pm.height() / pm.devicePixelRatio();
    p.drawText(QRectF(8, h, pm.width() / pm.devicePixelRatio() - 16, extra), Qt::AlignVCenter | Qt::AlignLeft, footer);
    return img;
}

}  // namespace abss::gui
