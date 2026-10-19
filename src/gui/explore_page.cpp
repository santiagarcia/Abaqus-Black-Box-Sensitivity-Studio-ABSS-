#include "abss/gui/explore_page.hpp"

#include "abss/gui/theme.hpp"
#include "abss/gui/widgets.hpp"
#include "abss/viz/colormap.hpp"
#include "abss/viz/scene.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTextStream>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

namespace abss::gui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

void setItemEnabled(QComboBox* c, int row, bool on, const QString& tip = {}) {
    if (auto* m = qobject_cast<QStandardItemModel*>(c->model())) {
        if (auto* it = m->item(row)) {
            it->setEnabled(on);
            it->setToolTip(tip);
        }
    }
}

QString encode(const viz::OutputChoice& o) { return QString("%1/%2/%3").arg(o.field).arg(o.component).arg(o.magnitude ? 1 : 0); }
viz::OutputChoice decode(const QString& s) {
    const auto p = s.split('/');
    viz::OutputChoice o;
    if (p.size() == 3) {
        o.field = p[0].toInt();
        o.component = p[1].toInt();
        o.magnitude = p[2].toInt() != 0;
    }
    return o;
}

QLabel* secondaryLabel(const QString& t) {
    auto* l = new QLabel(t);
    l->setProperty("secondary", true);
    return l;
}

QString kindText(const Element& e) {
    switch (e.info.topoDim) {
        case 3: return "3D solid";
        case 2: return e.info.family == ElementFamily::Shell || e.info.family == ElementFamily::Membrane ? "shell / 2D" : "2D solid";
        case 1: return "beam / truss";
        default: return "not drawn";
    }
}

QLabel* bannerLabel(QFrame* f, const QString& text, QStyle::StandardPixmap icon) {
    auto* lay = new QHBoxLayout(f);
    lay->setContentsMargins(10, 6, 10, 6);
    auto* ic = new QLabel;
    ic->setPixmap(QApplication::style()->standardIcon(icon).pixmap(18, 18));
    lay->addWidget(ic);
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    lay->addWidget(l, 1);
    return l;
}
}  // namespace

ExplorePage::ExplorePage(Controller* controller, QWidget* parent) : QWidget(parent), controller_(controller) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    demoBanner_ = new QFrame;
    demoBanner_->setObjectName("demoBanner");
    bannerLabel(demoBanner_, "Demo data — computed from an analytic model, not from Abaqus. Every value, chart and export from "
                             "this result is marked synthetic.",
                QStyle::SP_MessageBoxWarning);
    staleBanner_ = new QFrame;
    staleBanner_->setObjectName("infoBanner");
    bannerLabel(staleBanner_, "Results are from the previous setup. Run again to update.", QStyle::SP_MessageBoxInformation);
    auto* bannerWrap = new QWidget;
    auto* bw = new QVBoxLayout(bannerWrap);
    bw->setContentsMargins(12, 8, 12, 0);
    bw->setSpacing(6);
    bw->addWidget(demoBanner_);
    bw->addWidget(staleBanner_);
    lay->addWidget(bannerWrap);
    lay->addWidget(buildWhatBar());

    inner_ = new QMainWindow;
    inner_->setWindowFlags(Qt::Widget);
    inner_->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);
    auto* center = new QWidget;
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(buildViewToolbar());
    view_ = new VtkView;
    cl->addWidget(view_, 1);
    cl->addWidget(buildTimeline());
    inner_->setCentralWidget(center);

    auto* displayDock = new QDockWidget("Display", inner_);
    displayDock->setObjectName("displayDock");
    displayDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    displayDock->setWidget(buildDisplayDock());
    inner_->addDockWidget(Qt::RightDockWidgetArea, displayDock);

    auto* modelDock = new QDockWidget("Model", inner_);
    modelDock->setObjectName("modelDock");
    modelDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setMinimumWidth(210);
    modelDock->setWidget(tree_);
    inner_->addDockWidget(Qt::LeftDockWidgetArea, modelDock);

    auto* historyDock = new QDockWidget("History at picked points", inner_);
    historyDock->setObjectName("historyDock");
    historyDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    historyDock->setWidget(buildHistoryPanel());
    inner_->addDockWidget(Qt::BottomDockWidgetArea, historyDock);
    inner_->resizeDocks({historyDock}, {260}, Qt::Vertical);
    inner_->resizeDocks({displayDock}, {270}, Qt::Horizontal);
    inner_->resizeDocks({modelDock}, {220}, Qt::Horizontal);
    lay->addWidget(inner_, 1);

    connect(view_, &VtkView::picked, this, &ExplorePage::onPicked);
    connect(view_, &VtkView::keyAction, this, &ExplorePage::onKeyAction);
    connect(&play_, &QTimer::timeout, this, [this]() { stepFrame(+1); });
    demoBanner_->hide();
    staleBanner_->hide();
    applyTheme();
    setEnabled(false);
}

// ---- construction --------------------------------------------------------------------------------------------

QWidget* ExplorePage::buildWhatBar() {
    auto* bar = new QFrame;
    bar->setObjectName("whatBar");
    auto* v = new QVBoxLayout(bar);
    v->setContentsMargins(12, 8, 12, 8);
    v->setSpacing(6);
    auto* h = new QHBoxLayout;
    h->setSpacing(8);
    auto lab = [](const QString& t) {
        auto* l = new QLabel(t);
        l->setStyleSheet("font-weight: 600;");
        return l;
    };
    output_ = new QComboBox;
    output_->setMinimumWidth(150);
    output_->setToolTip("Which result to color the model by.");
    where_ = new QComboBox;
    where_->setMinimumWidth(270);
    where_->setToolTip("Where the value lives. Integration-point values are shown per element: either their average "
                       "(display only; the stored data keeps every point) or one chosen point.");
    param_combo_ = new QComboBox;
    param_combo_->setMinimumWidth(270);
    param_combo_->setToolTip("The parameter the derivative is taken with respect to.");
    h->addWidget(lab("Output"));
    h->addWidget(output_);
    h->addSpacing(6);
    h->addWidget(lab("Where"));
    h->addWidget(where_);
    h->addSpacing(6);
    h->addWidget(lab("Parameter"));
    h->addWidget(param_combo_);
    h->addSpacing(6);
    h->addWidget(lab("Order"));
    orderGroup_ = new QButtonGroup(this);
    orderGroup_->setExclusive(true);
    const char* tips[] = {"Order 0: the nominal result itself.", "Order 1: rate of change (first derivative).",
                          "Order 2: curvature (second derivative).",
                          "Order 3: third derivative. Experimental: computed, but not confirmed by every validation test."};
    for (int k = 0; k <= 3; ++k) {
        auto* b = new QToolButton;
        b->setText(QString::number(k));
        b->setCheckable(true);
        b->setProperty("segment", true);
        b->setToolTip(tips[k]);
        b->setAccessibleName(QString("Derivative order %1").arg(k));
        orderGroup_->addButton(b, k);
        orderButtons_.push_back(b);
        h->addWidget(b);
    }
    h->addSpacing(8);
    qualityBadge_ = new QLabel;
    syntheticBadge_ = new QLabel("Synthetic");
    styleBadge(syntheticBadge_, "demo");
    syntheticBadge_->setToolTip("These numbers come from the bundled analytic demo model, not from Abaqus.");
    evidenceBtn_ = new QToolButton;
    evidenceBtn_->setText("Evidence…");
    evidenceBtn_->setToolTip("Show the validation evidence and notes recorded for this derivative.");
    suspect_ = new QLabel;
    h->addStretch(1);
    v->addLayout(h);
    auto* h2 = new QHBoxLayout;
    h2->setSpacing(8);
    question_ = new QLabel;
    question_->setObjectName("question");
    question_->setWordWrap(true);
    question_->setToolTip("Total derivative: includes how the whole loading history responds to the change.");
    projection_ = secondaryLabel({});
    h2->addWidget(question_, 1);
    h2->addWidget(lab("Quality"));
    h2->addWidget(qualityBadge_);
    h2->addWidget(syntheticBadge_);
    h2->addWidget(evidenceBtn_);
    v->addLayout(h2);
    auto* h3 = new QHBoxLayout;
    h3->addWidget(projection_, 1);
    h3->addWidget(suspect_);
    v->addLayout(h3);
    for (auto* c : {output_, where_, param_combo_}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumWidth(0);
    }
    output_->setMinimumContentsLength(12);
    where_->setMinimumContentsLength(26);
    param_combo_->setMinimumContentsLength(30);

    connect(output_, &QComboBox::currentIndexChanged, this, [this]() { onSelectionChanged(); });
    connect(where_, &QComboBox::currentIndexChanged, this, [this]() { onSelectionChanged(); });
    connect(param_combo_, &QComboBox::currentIndexChanged, this, [this]() {
        if (updating_) return;
        param_ = param_combo_->currentData().toInt();
        updateOrderButtons();
        onSelectionChanged();
    });
    connect(orderGroup_, &QButtonGroup::idClicked, this, [this](int k) {
        order_ = k;
        onSelectionChanged();
    });
    connect(evidenceBtn_, &QToolButton::clicked, this, [this]() {
        if (!store_) return;
        const auto d = currentDerivative();
        QString text;
        if (d.order() == 0) {
            text = "Order 0 shows the nominal result; there is no derivative evidence to show.";
        } else {
            auto it = store_->quality.find(d);
            if (it == store_->quality.end()) {
                text = "No quality record exists for this derivative.";
            } else {
                const auto& q = it->second;
                text = QString("<b>%1</b>: %2<br><br>").arg(qs(viz::notationLabel(*store_, spec_.output, d))).arg(qualityName(q.status));
                if (store_->synthetic) text += "<i>Synthetic demo data — computed from an analytic model, not from Abaqus.</i><br><br>";
                auto num = [](double v) { return v < 0 ? QString("not run") : QString::number(v, 'g', 3); };
                text += "Max relative error against full re-analyses: " + num(q.fdRelativeError) + "<br>";
                text += "Tangent check error: " + num(q.tangentCheckError) + "<br>";
                text += "Predictor (commit) residual ratio: " + num(q.predictorResidualRatio) + "<br>";
                text += "Perturbation step h: " + QString::number(q.stepSize, 'g', 4) + "<br>";
                text += "Scheme: " + qs(q.scheme).toHtmlEscaped() + "<br>";
                if (!q.suspectFrames.empty()) {
                    QStringList s;
                    for (int f : q.suspectFrames) s << QString::number(f);
                    text += "<br><b>Suspect increments</b> (possibly non-smooth): " + s.join(", ") + "<br>";
                }
                if (!q.notes.empty()) {
                    text += "<br><b>Notes</b><ul>";
                    for (const auto& n : q.notes) text += "<li>" + qs(n).toHtmlEscaped() + "</li>";
                    text += "</ul>";
                }
            }
        }
        QMessageBox box(this);
        box.setWindowTitle("Evidence for this result");
        box.setTextFormat(Qt::RichText);
        box.setText(text);
        box.exec();
    });
    return bar;
}

QWidget* ExplorePage::buildViewToolbar() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(8, 4, 8, 4);
    h->setSpacing(4);
    auto* hint = secondaryLabel("Drag: rotate · right-drag: pan · wheel: zoom · click: history (Ctrl+click adds)");
    h->addWidget(hint, 1);
    auto add = [&](ViewIcon icon, const QString& tip, const QString& action) {
        auto* b = new QToolButton;
        b->setIcon(viewIcon(icon));
        b->setIconSize(QSize(20, 20));
        b->setToolTip(tip);
        b->setAccessibleName(tip.section('(', 0, 0).trimmed());
        b->setAutoRaise(false);
        h->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, action]() { onKeyAction(action); });
        return b;
    };
    add(ViewIcon::Fit, "Fit the model in the view (F)", "fit");
    add(ViewIcon::Iso, "Isometric view (I)", "iso");
    add(ViewIcon::XY, "Look at the XY plane (1)", "xy");
    add(ViewIcon::XZ, "Look at the XZ plane (2)", "xz");
    add(ViewIcon::YZ, "Look at the YZ plane (3)", "yz");
    orthoBtn_ = add(ViewIcon::Perspective, "Orthographic projection on/off (P)", "projection");
    orthoBtn_->setCheckable(true);
    auto* shot = add(ViewIcon::Camera, "Save a screenshot of the 3D view (Ctrl+Shift+S)", "screenshot");
    Q_UNUSED(shot);
    return w;
}

QWidget* ExplorePage::buildDisplayDock() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(12);

    auto* shape = new QGroupBox("Shape");
    auto* sf = new QFormLayout(shape);
    deformed_ = new QCheckBox("Show deformed shape");
    deformed_->setToolTip("Draw the model at its nominal displaced position (D).");
    scale_ = new QDoubleSpinBox;
    scale_->setRange(0.0, 1e6);
    scale_->setDecimals(3);
    scale_->setValue(1.0);
    scale_->setToolTip("Deformation scale factor (1 = true scale).");
    autoScale_ = new QPushButton("Auto");
    autoScale_->setToolTip("Scale so the largest displacement over all increments is 10 % of the model size.");
    auto* scaleRow = new QHBoxLayout;
    scaleRow->addWidget(scale_, 1);
    scaleRow->addWidget(autoScale_);
    undeformed_ = new QCheckBox("Show undeformed outline");
    undeformed_->setToolTip("Overlay the undeformed mesh as a light wireframe.");
    sf->addRow(deformed_);
    sf->addRow("Scale", scaleRow);
    sf->addRow(undeformed_);
    v->addWidget(shape);

    auto* mesh = new QGroupBox("Mesh");
    auto* mf = new QFormLayout(mesh);
    edges_ = new QCheckBox("Show element edges");
    edges_->setChecked(true);
    edges_->setToolTip("Draw element edges (E).");
    wire_ = new QCheckBox("Wireframe");
    wire_->setToolTip("Draw only the element edges, colored by the selected result.");
    opacity_ = new QSlider(Qt::Horizontal);
    opacity_->setRange(10, 100);
    opacity_->setValue(100);
    opacity_->setToolTip("Surface opacity (100 % = opaque).");
    mf->addRow(edges_);
    mf->addRow(wire_);
    mf->addRow("Opacity", opacity_);
    v->addWidget(mesh);

    auto* colors = new QGroupBox("Colors");
    auto* cf = new QFormLayout(colors);
    cmap_ = new QComboBox;
    for (const auto& m : viz::builtinColormaps()) cmap_->addItem(colormapSwatch(m), qs(m.name), qs(m.name));
    cmap_->setIconSize(QSize(64, 12));
    cmap_->insertSeparator(cmap_->count());
    cmap_->addItem("Load custom palette…", "__load__");
    cmap_->setToolTip("Color map. Diverging maps (Cool-warm, Seismic, Blue-white-red) suit signed values centered at zero.");
    cf->addRow("Color map", cmap_);
    rangeAll_ = new QRadioButton("All increments");
    rangeAll_->setToolTip("Fixed range covering every increment, so colors are comparable while playing.");
    rangeCur_ = new QRadioButton("This increment");
    rangeCur_->setToolTip("Range of the current increment only.");
    rangeMan_ = new QRadioButton("Fixed");
    rangeMan_->setToolTip("Enter the minimum and maximum yourself.");
    rangeAll_->setChecked(true);
    auto* rg = new QButtonGroup(this);
    rg->addButton(rangeAll_);
    rg->addButton(rangeCur_);
    rg->addButton(rangeMan_);
    auto* rv = new QVBoxLayout;
    rv->setSpacing(2);
    rv->addWidget(rangeAll_);
    rv->addWidget(rangeCur_);
    rv->addWidget(rangeMan_);
    cf->addRow("Range", rv);
    min_ = new QLineEdit;
    max_ = new QLineEdit;
    for (auto* e : {min_, max_}) {
        auto* val = new QDoubleValidator(e);
        val->setNotation(QDoubleValidator::ScientificNotation);
        e->setValidator(val);
        e->setEnabled(false);
        e->setToolTip("Editable when Range = Fixed.");
    }
    cf->addRow("Min", min_);
    cf->addRow("Max", max_);
    centered_ = new QCheckBox("Centered at zero");
    centered_->setToolTip("Symmetric range ±max|value| so zero is the neutral middle color.");
    cf->addRow(centered_);
    scaleMode_ = new QComboBox;
    scaleMode_->addItems({"Linear", "Logarithmic", "Symmetric log"});
    scaleMode_->setToolTip("Color scale. Symmetric log keeps the sign and compresses large magnitudes "
                           "(linear within 0.1 % of the largest magnitude).");
    cf->addRow("Scale", scaleMode_);
    rangeInfo_ = secondaryLabel({});
    rangeInfo_->setWordWrap(true);
    cf->addRow(rangeInfo_);
    v->addWidget(colors);

    auto* exp = new QGroupBox("Export");
    auto* ev = new QVBoxLayout(exp);
    auto* b1 = new QPushButton("Screenshot (PNG)…");
    b1->setToolTip("Save the 3D view as a PNG image.");
    auto* b2 = new QPushButton("Values at this increment (CSV)…");
    b2->setToolTip("Save the displayed value of every node or element at the current increment.");
    auto* b3 = new QPushButton("This increment for ParaView (VTU)…");
    b3->setToolTip("Save the mesh with every field and derivative of the current increment as a .vtu file.");
    ev->addWidget(b1);
    ev->addWidget(b2);
    ev->addWidget(b3);
    v->addWidget(exp);
    v->addStretch(1);
    scroll->setWidget(body);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(290);

    connect(b1, &QPushButton::clicked, this, &ExplorePage::exportScreenshot);
    connect(b2, &QPushButton::clicked, this, &ExplorePage::exportFrameCsv);
    connect(b3, &QPushButton::clicked, this, &ExplorePage::exportVtu);
    connect(deformed_, &QCheckBox::toggled, this, [this](bool on) {
        scale_->setEnabled(on);
        autoScale_->setEnabled(on);
        undeformed_->setEnabled(on);
        view_->scene().setShowUndeformed(on && undeformed_->isChecked());
        refreshView(false);
    });
    connect(scale_, &QDoubleSpinBox::valueChanged, this, [this]() { refreshView(false); });
    connect(autoScale_, &QPushButton::clicked, this, [this]() {
        if (!store_) return;
        double umax = 0;
        for (size_t f = 0; f < store_->frames.size(); ++f) {
            const auto d = viz::nodalDisplacement(*store_, static_cast<int>(f));
            for (size_t i = 0; i + 2 < d.size(); i += 3) umax = std::max(umax, std::sqrt(d[i] * d[i] + d[i + 1] * d[i + 1] + d[i + 2] * d[i + 2]));
        }
        const double L = store_->mesh.characteristicLength();
        if (umax > 0 && L > 0) scale_->setValue(0.1 * L / umax);
    });
    connect(undeformed_, &QCheckBox::toggled, this, [this](bool on) {
        view_->scene().setShowUndeformed(on && deformed_->isChecked());
        view_->render();
    });
    connect(edges_, &QCheckBox::toggled, this, [this](bool on) {
        view_->scene().setEdges(on);
        view_->render();
    });
    connect(wire_, &QCheckBox::toggled, this, [this](bool on) {
        view_->scene().setWireframe(on);
        edges_->setEnabled(!on);
        view_->render();
    });
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
        view_->scene().setOpacity(v / 100.0);
        view_->render();
    });
    connect(cmap_, &QComboBox::activated, this, [this](int idx) {
        if (cmap_->itemData(idx).toString() == "__load__") {
            const QString path = QFileDialog::getOpenFileName(this, "Load a color palette", QString(),
                                                              "Palette (*.csv *.txt);;All files (*)");
            QString previous = userColormap_.count(colormapKey()) ? userColormap_[colormapKey()] : QString(viz::kDefaultSignedMap);
            if (path.isEmpty()) {
                cmap_->setCurrentIndex(cmap_->findData(previous));
                return;
            }
            QFile f(path);
            try {
                if (!f.open(QIODevice::ReadOnly)) throw std::invalid_argument("The file could not be opened.");
                auto m = viz::parseColormapText(f.readAll().toStdString(), QFileInfo(path).completeBaseName().toStdString());
                viz::registerCustomColormap(m);
                const QString name = qs(m.name);
                if (cmap_->findData(name) < 0) cmap_->insertItem(cmap_->count() - 2, colormapSwatch(m), name + " (custom)", name);
                cmap_->setCurrentIndex(cmap_->findData(name));
            } catch (const std::exception& e) {
                QMessageBox::warning(this, "This palette can't be used",
                                     QString("The palette file could not be read: %1\n\nWhat you can do:\n• Use one color per line "
                                             "as r,g,b (0–1 or 0–255) or t,r,g,b\n• Hex colors like #1F5FBF also work")
                                         .arg(QString::fromUtf8(e.what())));
                cmap_->setCurrentIndex(cmap_->findData(previous));
                return;
            }
        }
        userColormap_[colormapKey()] = cmap_->currentData().toString();
        refreshView(false);
    });
    auto rangeChanged = [this]() {
        const bool man = rangeMan_->isChecked();
        min_->setEnabled(man);
        max_->setEnabled(man);
        refreshView(true);
    };
    connect(rangeAll_, &QRadioButton::toggled, this, rangeChanged);
    connect(rangeCur_, &QRadioButton::toggled, this, rangeChanged);
    connect(min_, &QLineEdit::editingFinished, this, [this]() { refreshView(true); });
    connect(max_, &QLineEdit::editingFinished, this, [this]() { refreshView(true); });
    connect(centered_, &QCheckBox::toggled, this, [this]() { refreshView(true); });
    connect(scaleMode_, &QComboBox::currentIndexChanged, this, [this]() { refreshView(true); });
    deformed_->setChecked(true);
    return scroll;
}

QWidget* ExplorePage::buildTimeline() {
    timeline_ = new QFrame;
    timeline_->setObjectName("whatBar");
    auto* h = new QHBoxLayout(timeline_);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(4);
    auto mk = [&](MediaIcon sp, const QString& tip) {
        auto* b = new QToolButton;
        b->setIcon(mediaIcon(sp));
        b->setIconSize(QSize(18, 18));
        b->setProperty("media", static_cast<int>(sp));
        b->setToolTip(tip);
        b->setAccessibleName(tip.section('(', 0, 0).trimmed());
        h->addWidget(b);
        return b;
    };
    first_ = mk(MediaIcon::First, "First increment (Home)");
    prev_ = mk(MediaIcon::Prev, "Previous increment (←)");
    playBtn_ = mk(MediaIcon::Play, "Play / pause (Space)");
    next_ = mk(MediaIcon::Next, "Next increment (→)");
    last_ = mk(MediaIcon::Last, "Last increment (End)");
    speed_ = new QComboBox;
    speed_->addItem("Slow", 1000);
    speed_->addItem("Normal", 500);
    speed_->addItem("Fast", 200);
    speed_->setCurrentIndex(1);
    speed_->setToolTip("Playback speed.");
    h->addWidget(speed_);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setMinimumWidth(160);
    slider_->setToolTip("Increment");
    slider_->setTickPosition(QSlider::TicksBelow);
    h->addWidget(slider_, 1);
    frameLabel_ = new QLabel;
    frameLabel_->setMinimumWidth(300);
    h->addWidget(frameLabel_, 3);
    connect(first_, &QToolButton::clicked, this, [this]() { setFrame(0); });
    connect(last_, &QToolButton::clicked, this, [this]() { if (store_) setFrame(static_cast<int>(store_->frames.size()) - 1); });
    connect(prev_, &QToolButton::clicked, this, [this]() { stepFrame(-1); });
    connect(next_, &QToolButton::clicked, this, [this]() { stepFrame(+1); });
    connect(playBtn_, &QToolButton::clicked, this, &ExplorePage::togglePlay);
    connect(speed_, &QComboBox::currentIndexChanged, this, [this]() { play_.setInterval(speed_->currentData().toInt()); });
    connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        if (!updating_) setFrame(v);
    });
    play_.setInterval(500);
    return timeline_;
}

QWidget* ExplorePage::buildHistoryPanel() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 4, 8, 8);
    v->setSpacing(4);
    auto* h = new QHBoxLayout;
    h->addWidget(new QLabel("X axis"));
    axis_ = new QComboBox;
    axis_->addItem("Total time", static_cast<int>(HistoryAxis::TotalTime));
    axis_->addItem("Step time", static_cast<int>(HistoryAxis::StepTime));
    axis_->addItem("Increment", static_cast<int>(HistoryAxis::Increment));
    axis_->setToolTip("Horizontal axis of the history chart. Total time falls back to the increment number when it is "
                      "not increasing.");
    h->addWidget(axis_);
    nominal_ = new QCheckBox("Also show nominal value (right axis)");
    nominal_->setToolTip("Overlay the order-0 history of the same output on a secondary axis.");
    h->addWidget(nominal_);
    h->addStretch(1);
    clear_ = new QPushButton("Clear points");
    copy_ = new QPushButton("Copy data");
    csv_ = new QPushButton("Export CSV…");
    png_ = new QPushButton("Save chart (PNG)…");
    for (auto* b : {clear_, copy_, csv_, png_}) {
        b->setEnabled(false);
        b->setToolTip("Click on the model first to choose a point.");
        h->addWidget(b);
    }
    v->addLayout(h);
    chart_ = new HistoryChart;
    chart_->setMinimumHeight(200);
    v->addWidget(chart_, 1);
    connect(axis_, &QComboBox::currentIndexChanged, this, [this]() { refreshChart(); });
    connect(nominal_, &QCheckBox::toggled, this, [this]() { refreshChart(); });
    connect(clear_, &QPushButton::clicked, this, [this]() { onKeyAction("clearpicks"); });
    connect(copy_, &QPushButton::clicked, this, &ExplorePage::copyHistory);
    connect(csv_, &QPushButton::clicked, this, &ExplorePage::exportHistoryCsv);
    connect(png_, &QPushButton::clicked, this, &ExplorePage::exportChartPng);
    connect(chart_, &HistoryChart::xClicked, this, [this](double x) {
        if (!store_) return;
        const auto xs = historyAxis(*store_, static_cast<HistoryAxis>(axis_->currentData().toInt()));
        int best = 0;
        for (size_t i = 0; i < xs.size(); ++i)
            if (std::abs(xs[i] - x) < std::abs(xs[static_cast<size_t>(best)] - x)) best = static_cast<int>(i);
        setFrame(best);
    });
    return w;
}

// ---- state -----------------------------------------------------------------------------------------------------

void ExplorePage::applyTheme() {
    view_->scene().setTheme(tokens().dark);
    chart_->applyTheme();
    for (int i = 0; i < cmap_->count(); ++i)
        if (const auto* m = viz::findColormap(cmap_->itemData(i).toString().toStdString())) cmap_->setItemIcon(i, colormapSwatch(*m));
    suspect_->setStyleSheet(QString("color: %1; font-weight: 600;").arg(tokens().warn.name()));
    styleBadge(syntheticBadge_, "demo");
    for (auto* b : {first_, prev_, next_, last_}) b->setIcon(mediaIcon(static_cast<MediaIcon>(b->property("media").toInt())));
    playBtn_->setIcon(mediaIcon(play_.isActive() ? MediaIcon::Pause : MediaIcon::Play));
    if (store_) updateQuality();
    if (store_) {
        refreshView(false);
        refreshChart();
        refreshMarkers();
    }
    view_->render();
}

void ExplorePage::setStaleBanner(bool on) { staleBanner_->setVisible(on); }

DerivativeSpec ExplorePage::currentDerivative() const {
    if (order_ <= 0 || param_ < 0) return DerivativeSpec::nominal();
    return DerivativeSpec::pure(param_, order_);
}

void ExplorePage::setStore(std::shared_ptr<const ResultStore> store) {
    play_.stop();
    playBtn_->setIcon(mediaIcon(MediaIcon::Play));
    const bool meshChanged = !store_ || !store || store_->mesh.nodeCount() != store->mesh.nodeCount() ||
                             store_->mesh.elementCount() != store->mesh.elementCount() || store_->mesh.xyz != store->mesh.xyz;
    store_ = std::move(store);
    picks_.clear();
    allRangeValid_ = false;
    setEnabled(static_cast<bool>(store_));
    if (!store_) {
        chart_->showEmpty("No results yet. Run an analysis or try the demo.");
        return;
    }
    demoBanner_->setVisible(store_->synthetic);
    syntheticBadge_->setVisible(store_->synthetic);
    staleBanner_->setVisible(controller_ && controller_->resultsStale());
    auto& sc = view_->scene();
    if (meshChanged) {
        sc.setMesh(store_->mesh);
        edges_->setChecked(store_->mesh.elementCount() * 4 < 150000);
        sc.setEdges(edges_->isChecked());
        sc.setWireframe(wire_->isChecked());
        sc.setOpacity(opacity_->value() / 100.0);
    }
    sc.setWatermark(store_->synthetic ? "DEMO — synthetic data" : "");
    // Defaults: first parameter with derivatives, order 1; else nominal.
    param_ = -1;
    order_ = 0;
    for (const auto& d : store_->derivatives())
        if (d.order() >= 1 && d.isPure()) {
            param_ = d.params.front();
            order_ = 1;
            break;
        }
    const int nFrames = static_cast<int>(store_->frames.size());
    frame_ = std::max(0, nFrames - 1);
    // Default output: U component with the largest nominal magnitude at the last increment (order >= 1),
    // S_Mises (or |U|) for nominal-only results.
    viz::OutputChoice def;
    const int fu = store_->fieldIndex("U");
    if (order_ >= 1 && fu >= 0) {
        def = {fu, 0, false};
        double best = -1;
        if (const auto* v = store_->get(fu, DerivativeSpec::nominal(), frame_)) {
            const int nc = store_->fields[static_cast<size_t>(fu)].componentCount();
            for (int c = 0; c < nc; ++c) {
                double m = 0;
                for (size_t i = static_cast<size_t>(c); i < v->size(); i += static_cast<size_t>(nc)) m = std::max(m, std::abs((*v)[i]));
                if (m > best) {
                    best = m;
                    def.component = c;
                }
            }
        }
    } else if (store_->fieldIndex("S_Mises") >= 0) {
        def = {store_->fieldIndex("S_Mises"), 0, false};
    } else if (fu >= 0) {
        def = {fu, 0, store_->fields[static_cast<size_t>(fu)].componentCount() > 1};
    } else if (!store_->fields.empty()) {
        def = {0, 0, false};
    }
    spec_ = {};
    spec_.output = def;
    spec_.derivative = currentDerivative();
    rebuildSelectors();
    updateModelTree();
    // Timeline
    updating_ = true;
    slider_->setRange(0, std::max(0, nFrames - 1));
    slider_->setValue(frame_);
    slider_->setTracking(store_->mesh.elementCount() <= 150000);
    updating_ = false;
    const bool multi = nFrames >= 2;
    for (QWidget* w : std::initializer_list<QWidget*>{first_, prev_, playBtn_, next_, last_, speed_, slider_}) w->setVisible(multi);
    const bool hasU = fu >= 0 && store_->has(fu, DerivativeSpec::nominal());
    deformed_->setEnabled(hasU);
    deformed_->setToolTip(hasU ? "Draw the model at its nominal displaced position (D)." : "This result has no displacement (U) output.");
    if (!hasU) deformed_->setChecked(false);
    scale_->setEnabled(hasU && deformed_->isChecked());
    autoScale_->setEnabled(hasU && deformed_->isChecked());
    undeformed_->setEnabled(hasU && deformed_->isChecked());
    onSelectionChanged();
    if (meshChanged || firstMesh_) {
        const bool planar = sc.isPlanar();
        sc.setParallelProjection(planar);
        orthoBtn_->setChecked(planar);
        sc.applyView(planar ? viz::Scene::View::XY : viz::Scene::View::Iso);
        firstMesh_ = false;
    }
    refreshChart();
    view_->render();
}

void ExplorePage::rebuildSelectors() {
    updating_ = true;
    param_combo_->clear();
    std::set<int> withDerivs;
    for (const auto& d : store_->derivatives())
        if (d.order() >= 1 && d.isPure()) withDerivs.insert(d.params.front());
    for (int p : withDerivs) {
        const auto& info = store_->parameters.at(static_cast<size_t>(p));
        QString text = qs(viz::parameterSymbol(*store_, p));
        if (!info.description.empty()) text += " — " + qs(info.description);
        bool experimental = false;
        for (const auto& [d, q] : store_->quality)
            if (d.isPure() && !d.params.empty() && d.params.front() == p && q.status == QualityStatus::Experimental) experimental = true;
        if (experimental) text += " — experimental";
        param_combo_->addItem(text, p);
    }
    if (withDerivs.empty()) {
        param_combo_->addItem("(no sensitivities in these results)", -1);
        param_combo_->setEnabled(false);
        param_combo_->setToolTip("These results contain only nominal values. Run with derivative order 1 or higher.");
    } else {
        param_combo_->setEnabled(order_ > 0);
        param_combo_->setToolTip(order_ > 0 ? "The parameter the derivative is taken with respect to."
                                            : "Order 0 shows the nominal result, which does not depend on a parameter choice.");
        const int idx = param_combo_->findData(param_);
        param_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
        param_ = param_combo_->currentData().toInt();
    }
    updating_ = false;
    updateOrderButtons();
    rebuildOutputCombo();
    rebuildWhereCombo();
}

void ExplorePage::updateOrderButtons() {
    if (!store_) return;
    for (int k = 0; k < static_cast<int>(orderButtons_.size()); ++k) {
        auto* b = orderButtons_[static_cast<size_t>(k)];
        bool ok = k == 0;
        if (k > 0 && param_ >= 0)
            for (size_t f = 0; f < store_->fields.size() && !ok; ++f) ok = store_->has(static_cast<int>(f), DerivativeSpec::pure(param_, k));
        b->setEnabled(ok);
        QString tip = k == 0 ? "Order 0: the nominal result itself."
                             : QString("Order %1 derivative with respect to the selected parameter.").arg(k);
        if (k >= 3) tip += " Experimental: computed, but not confirmed by every validation test. Use with care.";
        if (!ok) tip = "Not computed in this run. Set up ▸ Derivative order.";
        b->setToolTip(tip);
        b->setText(k >= 3 ? QString("%1 ⚠").arg(k) : QString::number(k));
    }
    if (!orderButtons_[static_cast<size_t>(std::clamp(order_, 0, 3))]->isEnabled()) order_ = 0;
    orderButtons_[static_cast<size_t>(std::clamp(order_, 0, 3))]->setChecked(true);
}

void ExplorePage::rebuildOutputCombo() {
    const bool prev = updating_;
    updating_ = true;
    output_->clear();
    const auto d = currentDerivative();
    int lastField = -1;
    int select = -1, firstAvail = -1;
    for (const auto& o : viz::outputChoices(*store_)) {
        if (lastField >= 0 && o.field != lastField) output_->insertSeparator(output_->count());
        lastField = o.field;
        const bool avail = viz::outputAvailable(*store_, o, d);
        QString text = qs(viz::quantityName(*store_, o));
        const auto& fd = store_->fields[static_cast<size_t>(o.field)];
        if (!avail) text += o.magnitude && d.order() > 0 ? "  (not offered for derivatives)" : "  (not computed)";
        output_->addItem(text, encode(o));
        const int row = output_->count() - 1;
        QString tip = qs(fd.description);
        if (o.magnitude && d.order() > 0)
            tip = "The derivative of a magnitude is not the magnitude of the derivative; choose a component.";
        else if (!avail)
            tip = "Not computed for this parameter and order.";
        setItemEnabled(output_, row, avail, tip);
        if (avail && firstAvail < 0) firstAvail = row;
        if (o == spec_.output && avail) select = row;
    }
    output_->setCurrentIndex(select >= 0 ? select : std::max(firstAvail, 0));
    spec_.output = decode(output_->currentData().toString());
    updating_ = prev;
}

void ExplorePage::rebuildWhereCombo() {
    const bool prev = updating_;
    updating_ = true;
    const QString old = where_->currentData().toString();
    where_->clear();
    const auto& fd = store_->fields.at(static_cast<size_t>(spec_.output.field));
    switch (fd.location) {
        case FieldLocation::Node:
            where_->addItem("Nodes", "node");
            where_->setEnabled(false);
            where_->setToolTip("This output is stored at the nodes.");
            break;
        case FieldLocation::Element:
            where_->addItem("Elements", "elem");
            where_->setEnabled(false);
            where_->setToolTip("This output is stored once per element.");
            break;
        case FieldLocation::IntegrationPoint: {
            where_->setEnabled(true);
            where_->setToolTip("Element average: Abaqus computes this value at points inside each element. The colors show "
                               "their average for display only; the stored data keeps each point.");
            where_->addItem("Elements (average of integration points)", "avg");
            const int n = viz::maxIntegrationPoints(*store_);
            for (int k = 1; k <= n; ++k) where_->addItem(QString("Integration point %1").arg(k), QString("ip%1").arg(k));
            const int i = where_->findData(old);
            where_->setCurrentIndex(i >= 0 ? i : 0);
            break;
        }
    }
    const QString cur = where_->currentData().toString();
    if (cur.startsWith("ip")) {
        spec_.projection = viz::IpProjection::SinglePoint;
        spec_.ipNumber = cur.mid(2).toInt();
    } else {
        spec_.projection = viz::IpProjection::ElementAverage;
    }
    updating_ = prev;
}

QString ExplorePage::colormapKey() const {
    if (!store_) return {};
    return qs(viz::quantityName(*store_, spec_.output)) + "#" + QString::number(order_);
}

void ExplorePage::onSelectionChanged() {
    if (updating_ || !store_) return;
    updating_ = true;
    param_combo_->setEnabled(order_ > 0 && param_combo_->count() > 0 && param_combo_->currentData().toInt() >= 0);
    param_combo_->setToolTip(order_ > 0 ? "The parameter the derivative is taken with respect to."
                                        : "Order 0 shows the nominal result, which does not depend on a parameter choice.");
    updating_ = false;
    // Output: keep the user's choice when available for the new derivative, else first available.
    const auto chosen = decode(output_->currentData().toString());
    if (chosen.field >= 0) spec_.output = chosen;
    spec_.derivative = currentDerivative();
    rebuildOutputCombo();
    rebuildWhereCombo();
    allRangeValid_ = false;
    applyDefaultsForSelection();
    updateQuality();
    refreshView(true);
    refreshMarkers();
    refreshChart();
}

void ExplorePage::applyDefaultsForSelection() {
    allRange_ = viz::rangeOverAllFrames(*store_, spec_);
    allRangeValid_ = true;
    const bool signedData = allRange_.valid() && allRange_.lo < 0 && allRange_.hi > 0;
    const bool sensitivity = order_ > 0;
    updating_ = true;
    QString map;
    auto it = userColormap_.find(colormapKey());
    if (it != userColormap_.end()) map = it->second;
    else map = (sensitivity || signedData) ? viz::kDefaultSignedMap : viz::kDefaultUnsignedMap;
    const int idx = cmap_->findData(map);
    if (idx >= 0) cmap_->setCurrentIndex(idx);
    centered_->setEnabled(sensitivity || signedData);
    centered_->setToolTip(centered_->isEnabled() ? "Symmetric range ±max|value| so zero is the neutral middle color."
                                                 : "Only useful when values can be negative and positive.");
    centered_->setChecked(sensitivity || signedData);
    const bool logOk = viz::logScaleValid(allRange_);
    setItemEnabled(scaleMode_, 1, logOk, logOk ? QString() : "Log scale needs all values greater than zero.");
    if (!logOk && scaleMode_->currentIndex() == 1) scaleMode_->setCurrentIndex(0);
    updating_ = false;
}

void ExplorePage::updateQuality() {
    const auto d = currentDerivative();
    suspect_->clear();
    suspect_->hide();
    if (d.order() == 0) {
        qualityBadge_->setText("Nominal");
        styleBadge(qualityBadge_, "neutral");
        qualityBadge_->setToolTip("Order 0: the nominal analysis result.");
        evidenceBtn_->setEnabled(false);
        evidenceBtn_->setToolTip("Order 0 has no derivative evidence.");
        return;
    }
    evidenceBtn_->setEnabled(true);
    evidenceBtn_->setToolTip("Show the validation evidence and notes recorded for this derivative.");
    auto it = store_->quality.find(d);
    if (it == store_->quality.end()) {
        qualityBadge_->setText("No record");
        styleBadge(qualityBadge_, "neutral");
        qualityBadge_->setToolTip("No quality record was stored for this derivative.");
        return;
    }
    const auto& q = it->second;
    QString kind = "neutral", tip;
    switch (q.status) {
        case QualityStatus::Validated:
            kind = "ok";
            tip = "Agreed with complete re-analyses at every increment within tolerance (max relative error " +
                  QString::number(q.fdRelativeError, 'g', 3) + ").";
            break;
        case QualityStatus::Experimental:
            kind = "warn";
            tip = "Computed, but not yet confirmed by every validation test. Use with care.";
            break;
        case QualityStatus::Unverified:
            kind = "neutral";
            tip = "Computed; not compared with complete re-analyses in this run.";
            break;
        case QualityStatus::Failed:
            kind = "error";
            tip = "Did not agree with complete re-analyses within tolerance. Do not rely on these values.";
            break;
        case QualityStatus::Synthetic:
            kind = "demo";
            tip = "Synthetic demo data.";
            break;
    }
    qualityBadge_->setText(qualityName(q.status));
    styleBadge(qualityBadge_, kind);
    qualityBadge_->setToolTip(tip + (q.notes.empty() ? QString() : "\n\n" + qs(q.notes.front())));
    if (!q.suspectFrames.empty()) {
        QStringList s;
        for (int f : q.suspectFrames) s << QString::number(f);
        suspect_->setText("Suspect increments: " + s.join(", "));
        suspect_->show();
        suspect_->setToolTip("Possible non-smooth events (yield onset, load reversal): derivatives there may not exist "
                             "and values are one-sided or unreliable.");
    }
}

viz::PickTarget ExplorePage::targetOf(const PickRec& p) const {
    const auto assoc = viz::associationOf(*store_, spec_.output.field);
    return {assoc, assoc == viz::DataAssoc::Point ? p.node : p.cell};
}

QString ExplorePage::footerText() const {
    if (!store_) return {};
    QStringList lines;
    lines << qs(viz::unitsLine(*store_, spec_.output, spec_.derivative));
    const auto dv = viz::extractDisplay(*store_, spec_, frame_);
    if (!dv.projectionNote.empty())
        lines << (spec_.projection == viz::IpProjection::ElementAverage ? QString("Element average of integration points (display only)")
                                                                        : QString("Integration point %1 only").arg(spec_.ipNumber));
    if (store_->synthetic) lines << "Synthetic demo data — not Abaqus results";
    return lines.join("\n");
}

void ExplorePage::refreshView(bool /*rangeChanged*/) {
    if (!store_ || updating_ || spec_.output.field < 0) return;
    auto& sc = view_->scene();
    const auto dv = viz::extractDisplay(*store_, spec_, frame_);
    sc.setScalars(dv);
    if (deformed_->isChecked()) sc.setDisplacement(viz::nodalDisplacement(*store_, frame_), scale_->value());
    else sc.setDisplacement({}, 0.0);
    sc.setShowUndeformed(deformed_->isChecked() && undeformed_->isChecked());

    if (!allRangeValid_) {
        allRange_ = viz::rangeOverAllFrames(*store_, spec_);
        allRangeValid_ = true;
    }
    viz::Range data;
    QString rangeNote;
    double lo = 0, hi = 1;
    bool constant = false;
    double constValue = 0;
    const bool centered = centered_->isEnabled() && centered_->isChecked();
    if (rangeMan_->isChecked()) {
        bool ok1 = false, ok2 = false;
        const double a = QLocale().toDouble(min_->text(), &ok1), b = QLocale().toDouble(max_->text(), &ok2);
        const double a2 = ok1 ? a : min_->text().toDouble(&ok1), b2 = ok2 ? b : max_->text().toDouble(&ok2);
        if (ok1 && ok2 && b2 > a2) {
            lo = a2;
            hi = b2;
            rangeNote = "Fixed range.";
        } else {
            data = viz::finiteRange(dv.values);
            const auto r = viz::resolveRange(data, centered);
            lo = r.lo;
            hi = r.hi;
            rangeNote = "Enter a minimum smaller than the maximum; showing this increment's range meanwhile.";
        }
    } else {
        data = rangeAll_->isChecked() ? allRange_ : viz::finiteRange(dv.values);
        const auto r = viz::resolveRange(data, centered);
        lo = r.lo;
        hi = r.hi;
        constant = r.constant;
        constValue = r.constantValue;
        if (r.empty) rangeNote = "No values to show at this increment.";
        updating_ = true;
        min_->setText(QString::number(lo, 'g', 6));
        max_->setText(QString::number(hi, 'g', 6));
        updating_ = false;
    }
    auto scale = viz::ScaleMode::Linear;
    if (scaleMode_->currentIndex() == 1) {
        if (lo > 0 && hi > lo) scale = viz::ScaleMode::Log;
        else rangeNote += (rangeNote.isEmpty() ? "" : " ") + QString("Log scale needs all values greater than zero; using linear.");
    } else if (scaleMode_->currentIndex() == 2) {
        scale = viz::ScaleMode::SymLog;
    }
    const viz::Colormap* map = viz::findColormap(cmap_->currentData().toString().toStdString());
    if (!map) map = viz::findColormap(viz::kDefaultUnsignedMap);
    sc.setColoring(*map, lo, hi, scale, toRgb(tokens().nodata));

    QString footer = footerText();
    if (constant) footer = QString("All values = %1 (constant)\n").arg(qs(viz::formatValue(constValue))) + footer;
    if (!dv.available) {
        footer = (spec_.derivative.order() > 0 && frame_ == 0
                      ? QString("No data at increment 0: sensitivities start at increment 1 (increment 0 is the initial state)\n")
                      : QString("No data at this increment\n")) +
                 footer;
    }
    if (dv.available && std::any_of(dv.values.begin(), dv.values.end(), [](double x) { return !(x == x); }))
        footer = "Grey: no data here\n" + footer;
    sc.setLegend(viz::notationLabel(*store_, spec_.output, spec_.derivative), footer.toStdString(), true);
    rangeInfo_->setText(QString("Shown range: %1 … %2%3").arg(qs(viz::formatValue(lo)), qs(viz::formatValue(hi)),
                                                                 rangeNote.isEmpty() ? QString() : "\n" + rangeNote));
    question_->setText(qs(viz::questionSentence(*store_, spec_.output, spec_.derivative, dv.assoc)));
    projection_->setText(dv.projectionNote.empty() ? QString() : qs(dv.projectionNote));
    projection_->setVisible(!dv.projectionNote.empty());
    projection_->setToolTip("Abaqus computes this value at points inside each element. The colors show their average for "
                            "display only; the stored data keeps each point.");
    refreshTimelineLabel();
    refreshMarkers();
    if (chart_->hasData() && store_) {
        const auto xs = historyAxis(*store_, static_cast<HistoryAxis>(axis_->currentData().toInt()));
        if (static_cast<size_t>(frame_) < xs.size()) chart_->setCursorX(xs[static_cast<size_t>(frame_)]);
    }
    view_->render();
}

void ExplorePage::refreshTimelineLabel() {
    if (!store_) return;
    QString s = store_->frames.size() < 2 ? "Single increment" : qs(viz::frameLabel(*store_, frame_));
    auto it = store_->quality.find(spec_.derivative);
    if (it != store_->quality.end()) {
        const auto& sf = it->second.suspectFrames;
        if (std::find(sf.begin(), sf.end(), frame_) != sf.end()) s += "  ·  ⚠ flagged as possibly non-smooth";
    }
    frameLabel_->setText(s);
}

void ExplorePage::refreshMarkers() {
    if (!store_) return;
    std::vector<std::array<double, 3>> pos;
    std::vector<std::string> labels;
    std::vector<viz::Rgb> colors;
    for (size_t i = 0; i < picks_.size(); ++i) {
        const auto t = targetOf(picks_[i]);
        pos.push_back(t.assoc == viz::DataAssoc::Point ? view_->scene().nodePosition(t.index) : view_->scene().cellCenter(t.index));
        labels.push_back(i < 26 ? std::string(1, static_cast<char>('A' + i)) : "P" + std::to_string(i + 1));
        colors.push_back(toRgb(categoricalColor(static_cast<int>(i))));
    }
    view_->scene().setPickMarkers(pos, labels, colors);
    const bool any = !picks_.empty();
    for (auto* b : {clear_, copy_, csv_, png_}) {
        b->setEnabled(any);
        b->setToolTip(any ? QString() : "Click on the model first to choose a point.");
    }
    clear_->setToolTip(any ? "Remove all picked points (Esc)." : clear_->toolTip());
    copy_->setToolTip(any ? "Copy the plotted histories as CSV text." : copy_->toolTip());
    csv_->setToolTip(any ? "Save the plotted histories as a CSV file." : csv_->toolTip());
    png_->setToolTip(any ? "Save the chart as a PNG image." : png_->toolTip());
    emit exportsChanged();
}

void ExplorePage::refreshChart() {
    if (!store_) return;
    if (picks_.empty()) {
        chart_->showEmpty("Click on the model to plot the history at that point. Ctrl+click adds more points.");
        return;
    }
    HistoryAxis used = HistoryAxis::Increment;
    const auto requested = static_cast<HistoryAxis>(axis_->currentData().toInt());
    const auto xs = historyAxis(*store_, requested, &used);
    QString xTitle = used == HistoryAxis::TotalTime ? "Total time" : used == HistoryAxis::StepTime ? "Step time" : "Increment";
    if (requested == HistoryAxis::TotalTime && used != requested) xTitle += " (total time is not increasing)";
    const QString label = qs(viz::notationLabel(*store_, spec_.output, spec_.derivative));
    bool dashed = false;
    QString suffix;
    if (auto it = store_->quality.find(spec_.derivative); it != store_->quality.end()) {
        dashed = it->second.status == QualityStatus::Experimental || it->second.status == QualityStatus::Failed;
        if (it->second.status == QualityStatus::Experimental) suffix = " (experimental)";
        if (it->second.status == QualityStatus::Failed) suffix = " (failed validation)";
    }
    std::vector<HistoryChart::Series> series;
    QStringList names;
    for (size_t i = 0; i < picks_.size(); ++i) {
        const auto t = targetOf(picks_[i]);
        const QString letter = i < 26 ? QString(QChar('A' + static_cast<int>(i))) : QString("P%1").arg(i + 1);
        const QString where = qs(viz::describeTarget(*store_, t));
        names << letter + " (" + where + ")";
        series.push_back({letter + ": " + where + suffix, viz::historyAt(*store_, spec_, t),
                          categoricalColor(static_cast<int>(i)), dashed, false});
    }
    QString y2;
    if (nominal_->isChecked() && spec_.derivative.order() > 0) {
        auto nom = spec_;
        nom.derivative = DerivativeSpec::nominal();
        y2 = qs(viz::notationLabel(*store_, nom.output, nom.derivative));
        for (size_t i = 0; i < picks_.size(); ++i) {
            const auto t = targetOf(picks_[i]);
            const QString letter = i < 26 ? QString(QChar('A' + static_cast<int>(i))) : QString("P%1").arg(i + 1);
            series.push_back({letter + ": " + y2, viz::historyAt(*store_, nom, t), categoricalColor(static_cast<int>(i)).lighter(125),
                              false, true});
        }
    }
    const QString title = label + " at " + (picks_.size() == 1 ? qs(viz::describeTarget(*store_, targetOf(picks_[0]))) : names.join(", ")) +
                          "   ·   " + qs(viz::unitsLine(*store_, spec_.output, spec_.derivative));
    chart_->setData(title, xTitle, label, y2, xs, series);
    if (static_cast<size_t>(frame_) < xs.size()) chart_->setCursorX(xs[static_cast<size_t>(frame_)]);
}

void ExplorePage::updateModelTree() {
    tree_->clear();
    if (!store_) return;
    auto* root = new QTreeWidgetItem(tree_, {qs(store_->title.empty() ? "Model" : store_->title)});
    root->setExpanded(true);
    const auto& m = store_->mesh;
    auto* mesh = new QTreeWidgetItem(root, {QString("Mesh: %1 nodes, %2 elements").arg(m.nodeCount()).arg(m.elementCount())});
    std::map<std::pair<int, std::string>, std::pair<int, QString>> types;
    for (const auto& e : m.elements) {
        auto& t = types[{e.instance, e.type}];
        t.first++;
        t.second = kindText(e);
    }
    std::map<int, QTreeWidgetItem*> instItems;
    for (const auto& [key, val] : types) {
        QTreeWidgetItem* parent = mesh;
        if (key.first >= 0) {
            auto& it = instItems[key.first];
            if (!it) {
                const QString nm = static_cast<size_t>(key.first) < m.instanceNames.size() ? qs(m.instanceNames[static_cast<size_t>(key.first)])
                                                                                          : QString("Instance %1").arg(key.first);
                it = new QTreeWidgetItem(mesh, {nm});
                it->setExpanded(true);
            }
            parent = it;
        }
        new QTreeWidgetItem(parent, {QString("%1 × %2 — %3").arg(qs(key.second)).arg(val.first).arg(val.second)});
    }
    mesh->setExpanded(true);
    auto* steps = new QTreeWidgetItem(root, {"Steps"});
    std::map<std::string, int> perStep;
    std::vector<std::string> order;
    for (size_t i = 1; i < store_->frames.size(); ++i) {
        const auto& n = store_->frames[i].stepName;
        if (!perStep.count(n)) order.push_back(n);
        perStep[n]++;
    }
    for (const auto& n : order) new QTreeWidgetItem(steps, {QString("%1 — %2 increments").arg(qs(n)).arg(perStep[n])});
    steps->setExpanded(true);
    auto* params = new QTreeWidgetItem(root, {"Parameters studied"});
    std::set<int> seen;
    for (const auto& [d, q] : store_->quality)
        if (d.isPure() && !d.params.empty())
            new QTreeWidgetItem(params, {QString("%1 · order %2 — %3").arg(qs(viz::parameterSymbol(*store_, d.params.front()))).arg(d.order()).arg(qualityName(q.status))});
    params->setExpanded(true);
    auto* src = new QTreeWidgetItem(root, {"Source: " + qs(store_->synthetic ? "synthetic demo model (not Abaqus)" : store_->backend)});
    Q_UNUSED(src);
}

void ExplorePage::setFrame(int f) {
    if (!store_) return;
    const int n = static_cast<int>(store_->frames.size());
    f = std::clamp(f, 0, std::max(0, n - 1));
    frame_ = f;
    updating_ = true;
    slider_->setValue(f);
    updating_ = false;
    refreshView(rangeCur_->isChecked());
}

void ExplorePage::stepFrame(int delta) {
    if (!store_) return;
    const int n = static_cast<int>(store_->frames.size());
    if (n < 2) return;
    int f = frame_ + delta;
    const int firstData = spec_.derivative.order() > 0 ? 1 : 0;
    if (f >= n) f = firstData;  // loop
    if (f < 0) f = n - 1;
    setFrame(f);
}

void ExplorePage::togglePlay() {
    if (!store_ || store_->frames.size() < 2) return;
    if (play_.isActive()) {
        play_.stop();
        playBtn_->setIcon(mediaIcon(MediaIcon::Play));
    } else {
        play_.start(speed_->currentData().toInt());
        playBtn_->setIcon(mediaIcon(MediaIcon::Pause));
    }
}

void ExplorePage::onPicked(int cell, int node, bool add) {
    if (!store_) return;
    PickRec r{cell, node};
    if (add) {
        bool exists = false;
        for (const auto& p : picks_) exists = exists || (p.cell == cell && p.node == node);
        if (!exists && picks_.size() < 8) picks_.push_back(r);
    } else {
        picks_ = {r};
    }
    refreshMarkers();
    refreshChart();
    const auto t = targetOf(r);
    const auto dv = viz::extractDisplay(*store_, spec_, frame_);
    const double v = t.index >= 0 && static_cast<size_t>(t.index) < dv.values.size() ? dv.values[static_cast<size_t>(t.index)] : NAN;
    emit statusMessage(QString("%1: %2 = %3").arg(qs(viz::describeTarget(*store_, t)), qs(viz::notationLabel(*store_, spec_.output, spec_.derivative)),
                                                 qs(viz::formatValue(v))));
    view_->render();
}

void ExplorePage::autoPick(int n) {
    if (!store_) return;
    const auto dv = viz::extractDisplay(*store_, spec_, frame_);
    std::vector<std::pair<double, int>> order;
    for (size_t i = 0; i < dv.values.size(); ++i)
        if (dv.values[i] == dv.values[i]) order.push_back({-std::abs(dv.values[i]), static_cast<int>(i)});
    std::sort(order.begin(), order.end());
    picks_.clear();
    for (const auto& [v, i] : order) {
        if (static_cast<int>(picks_.size()) >= n) break;
        PickRec r;
        if (dv.assoc == viz::DataAssoc::Point) {
            r.node = i;
            for (std::int32_t e = 0; e < store_->mesh.elementCount() && r.cell < 0; ++e)
                for (auto c : store_->mesh.elements[static_cast<size_t>(e)].conn)
                    if (c == i) {
                        r.cell = e;
                        break;
                    }
        } else {
            r.cell = i;
            r.node = store_->mesh.elements[static_cast<size_t>(i)].conn.empty() ? -1 : store_->mesh.elements[static_cast<size_t>(i)].conn.front();
        }
        picks_.push_back(r);
        // second point: prefer a location far from the first one, so the overlay is informative
        if (n > 1 && picks_.size() == 1 && order.size() > 2) {
            const auto& mid = order[order.size() / 2];
            PickRec r2;
            if (dv.assoc == viz::DataAssoc::Point) {
                r2.node = mid.second;
                for (std::int32_t e = 0; e < store_->mesh.elementCount() && r2.cell < 0; ++e)
                    for (auto c : store_->mesh.elements[static_cast<size_t>(e)].conn)
                        if (c == mid.second) {
                            r2.cell = e;
                            break;
                        }
            } else {
                r2.cell = mid.second;
                r2.node = store_->mesh.elements[static_cast<size_t>(mid.second)].conn.front();
            }
            picks_.push_back(r2);
        }
    }
    refreshMarkers();
    refreshChart();
    view_->render();
}

void ExplorePage::selectOutput(const QString& quantity) {
    if (!store_) return;
    for (int i = 0; i < output_->count(); ++i) {
        const auto o = decode(output_->itemData(i).toString());
        if (o.field >= 0 && qs(viz::quantityName(*store_, o)).compare(quantity, Qt::CaseInsensitive) == 0) {
            output_->setCurrentIndex(i);
            return;
        }
    }
}

void ExplorePage::selectDerivative(int param, int order) {
    if (!store_) return;
    const int idx = param_combo_->findData(param);
    if (idx >= 0) param_combo_->setCurrentIndex(idx);
    param_ = param;
    updateOrderButtons();
    if (order >= 0 && order < static_cast<int>(orderButtons_.size()) && orderButtons_[static_cast<size_t>(order)]->isEnabled()) {
        order_ = order;
        orderButtons_[static_cast<size_t>(order)]->setChecked(true);
    }
    onSelectionChanged();
}

void ExplorePage::onKeyAction(const QString& a) {
    if (!store_) return;
    auto& sc = view_->scene();
    if (a == "fit") sc.applyView(viz::Scene::View::Fit);
    else if (a == "iso") sc.applyView(viz::Scene::View::Iso);
    else if (a == "xy") sc.applyView(viz::Scene::View::XY);
    else if (a == "xz") sc.applyView(viz::Scene::View::XZ);
    else if (a == "yz") sc.applyView(viz::Scene::View::YZ);
    else if (a == "projection") {
        sc.setParallelProjection(!sc.parallelProjection());
        orthoBtn_->setChecked(sc.parallelProjection());
    } else if (a == "edges") edges_->setChecked(!edges_->isChecked());
    else if (a == "deformed") {
        if (deformed_->isEnabled()) deformed_->setChecked(!deformed_->isChecked());
    } else if (a == "play") togglePlay();
    else if (a == "prev") stepFrame(-1);
    else if (a == "next") stepFrame(+1);
    else if (a == "first") setFrame(0);
    else if (a == "last") setFrame(static_cast<int>(store_->frames.size()) - 1);
    else if (a == "clearpicks") {
        picks_.clear();
        refreshMarkers();
        refreshChart();
    } else if (a == "screenshot") exportScreenshot();
    view_->render();
}

// ---- exports ---------------------------------------------------------------------------------------------------

QString ExplorePage::saveName(const QString& suffix) const {
    QString base = store_ ? qs(store_->title) : QString("abss");
    base.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(dir).filePath(base + suffix);
}

QImage ExplorePage::viewImage(bool withFooter) {
    QImage img = view_->grabImage();
    if (!withFooter || !store_ || !store_->synthetic) return img;
    const int extra = static_cast<int>(28 * img.devicePixelRatio());
    QImage out(img.width(), img.height() + extra, QImage::Format_ARGB32);
    out.setDevicePixelRatio(img.devicePixelRatio());
    out.fill(tokens().bannerBg);
    QPainter p(&out);
    p.drawImage(QPoint(0, 0), img);
    p.setPen(tokens().bannerText);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    const double h = img.height() / img.devicePixelRatio();
    p.drawText(QRectF(10, h, img.width() / img.devicePixelRatio() - 20, 28), Qt::AlignVCenter | Qt::AlignLeft,
               "Synthetic demo data — computed from an analytic model, not from Abaqus");
    return out;
}

QImage ExplorePage::chartImage() {
    return chart_->renderImage(store_ && store_->synthetic ? "Synthetic demo data — computed from an analytic model, not from Abaqus"
                                                            : QString());
}

QString ExplorePage::currentHistoryCsv() const {
    if (!store_ || picks_.empty()) return {};
    std::vector<viz::PickTarget> targets;
    for (const auto& p : picks_) targets.push_back(targetOf(p));
    std::vector<viz::DisplaySpec> series = {spec_};
    if (nominal_->isChecked() && spec_.derivative.order() > 0) {
        auto nom = spec_;
        nom.derivative = DerivativeSpec::nominal();
        series.push_back(nom);
    }
    return historyCsv(*store_, targets, series);
}

namespace {
bool writeText(QWidget* parent, const QString& path, const QString& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(parent, "The file could not be saved",
                             "ABSS could not write " + QDir::toNativeSeparators(path) +
                                 ".\n\nWhat you can do:\n• Choose another folder\n• Close the file if another program has it open");
        return false;
    }
    f.write("\xEF\xBB\xBF");  // UTF-8 BOM so spreadsheet programs show ∂, ², ³ correctly
    f.write(text.toUtf8());
    return true;
}
}  // namespace

void ExplorePage::exportHistoryCsv() {
    if (picks_.empty()) return;
    const QString path = QFileDialog::getSaveFileName(this, "Export history as CSV", saveName("_history.csv"), "CSV (*.csv)");
    if (path.isEmpty()) return;
    if (writeText(this, path, currentHistoryCsv())) emit statusMessage("Saved " + QDir::toNativeSeparators(path));
}

void ExplorePage::copyHistory() {
    if (picks_.empty()) return;
    QApplication::clipboard()->setText(currentHistoryCsv());
    emit statusMessage("History data copied to the clipboard (CSV).");
}

void ExplorePage::exportChartPng() {
    if (picks_.empty()) return;
    const QString path = QFileDialog::getSaveFileName(this, "Save chart image", saveName("_history.png"), "PNG image (*.png)");
    if (path.isEmpty()) return;
    if (chartImage().save(path)) emit statusMessage("Saved " + QDir::toNativeSeparators(path));
}

void ExplorePage::exportScreenshot() {
    if (!store_) return;
    const QString path = QFileDialog::getSaveFileName(this, "Save screenshot", saveName("_view.png"), "PNG image (*.png)");
    if (path.isEmpty()) return;
    if (viewImage(true).save(path)) emit statusMessage("Saved " + QDir::toNativeSeparators(path));
}

void ExplorePage::exportFrameCsv() {
    if (!store_) return;
    const QString path = QFileDialog::getSaveFileName(this, "Export values at this increment", saveName(QString("_inc%1.csv").arg(frame_)),
                                                      "CSV (*.csv)");
    if (path.isEmpty()) return;
    if (writeText(this, path, frameCsv(*store_, spec_, frame_))) emit statusMessage("Saved " + QDir::toNativeSeparators(path));
}

void ExplorePage::exportVtu() {
    if (!store_) return;
    const QString path = QFileDialog::getSaveFileName(this, "Export this increment for ParaView", saveName(QString("_inc%1.vtu").arg(frame_)),
                                                      "VTK unstructured grid (*.vtu)");
    if (path.isEmpty()) return;
    try {
        viz::writeFrameVtu(*store_, frame_, path.toStdString());
        emit statusMessage("Saved " + QDir::toNativeSeparators(path));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "The file could not be saved", QString::fromUtf8(e.what()));
    }
}

}  // namespace abss::gui
