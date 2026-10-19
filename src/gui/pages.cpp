#include "abss/gui/pages.hpp"

#include "abss/gui/theme.hpp"
#include "abss/gui/widgets.hpp"
#include "abss/inp/deck.hpp"
#include "abss/linalg/parallel.hpp"
#include "abss/linalg/sparse.hpp"
#include "abss/viz/scene.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QTableWidget>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <set>

namespace abss::gui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

QLabel* title(const QString& t) {
    auto* l = new QLabel(t);
    l->setObjectName("pageTitle");
    return l;
}
QLabel* secondary(const QString& t, bool wrap = true) {
    auto* l = new QLabel(t);
    l->setProperty("secondary", true);
    l->setWordWrap(wrap);
    return l;
}
QPushButton* primary(const QString& t) {
    auto* b = new QPushButton(t);
    b->setProperty("primary", true);
    return b;
}
QFrame* card() {
    auto* f = new QFrame;
    f->setObjectName("card");
    return f;
}
QFrame* lockBanner(const QString& text) {
    auto* f = new QFrame;
    f->setObjectName("infoBanner");
    auto* h = new QHBoxLayout(f);
    h->setContentsMargins(10, 6, 10, 6);
    auto* ic = new QLabel;
    ic->setPixmap(QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(16, 16));
    h->addWidget(ic);
    h->addWidget(new QLabel(text), 1);
    f->hide();
    return f;
}
QString categoryLabel(ParamCategory c) {
    switch (c) {
        case ParamCategory::Material: return "Material";
        case ParamCategory::UserMaterial: return "User material (UMAT)";
        case ParamCategory::UserElement: return "User element (UEL)";
        case ParamCategory::Load: return "Load";
        case ParamCategory::Boundary: return "Boundary condition";
        case ParamCategory::Geometry: return "Geometry";
        case ParamCategory::Section: return "Section";
        case ParamCategory::Amplitude: return "Amplitude";
        case ParamCategory::Custom: return "Custom";
    }
    return "Other";
}
}  // namespace

// ---- ErrorPanel ------------------------------------------------------------------------------------------------

ErrorPanel::ErrorPanel(QWidget* parent) : QFrame(parent) {
    setObjectName("errorPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 10, 12, 10);
    auto* h = new QHBoxLayout;
    auto* ic = new QLabel;
    ic->setPixmap(QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(20, 20));
    h->addWidget(ic);
    title_ = new QLabel;
    title_->setStyleSheet("font-weight: 600; font-size: 11pt;");
    title_->setWordWrap(true);
    h->addWidget(title_, 1);
    v->addLayout(h);
    cause_ = new QLabel;
    cause_->setWordWrap(true);
    cause_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(cause_);
    actions_ = new QLabel;
    actions_->setWordWrap(true);
    actions_->setTextFormat(Qt::RichText);
    v->addWidget(actions_);
    toggle_ = new QToolButton;
    toggle_->setText("Show details");
    toggle_->setCheckable(true);
    v->addWidget(toggle_, 0, Qt::AlignLeft);
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setFont(monospaceFont());
    details_->setMaximumHeight(160);
    details_->hide();
    v->addWidget(details_);
    connect(toggle_, &QToolButton::toggled, this, [this](bool on) {
        details_->setVisible(on);
        toggle_->setText(on ? "Hide details" : "Show details");
    });
    hide();
}

void ErrorPanel::showError(const QString& t, const QString& cause, const QStringList& actions, const QString& details) {
    title_->setText(t);
    cause_->setText(cause);
    if (actions.isEmpty()) {
        actions_->hide();
    } else {
        QString html = "<b>What you can do</b><ul style='margin-top:2px'>";
        for (const auto& a : actions) html += "<li>" + a.toHtmlEscaped() + "</li>";
        actions_->setText(html + "</ul>");
        actions_->show();
    }
    details_->setPlainText(details);
    toggle_->setVisible(!details.trimmed().isEmpty());
    toggle_->setChecked(false);
    show();
}

// ---- StartPage -------------------------------------------------------------------------------------------------

StartPage::StartPage(Controller* c, QWidget* parent) : QWidget(parent), controller_(c) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(14);
    v->addWidget(title("Welcome to ABSS"));
    v->addWidget(secondary("Abaqus Black-Box Sensitivity Studio computes how every result of an Abaqus/Standard analysis — at "
                           "every accepted increment — changes when a model parameter changes. Start with an input file, a "
                           "saved results file, or the bundled demo."));
    error_ = new ErrorPanel;
    v->addWidget(error_);

    auto* grid = new QHBoxLayout;
    grid->setSpacing(16);
    auto* left = new QVBoxLayout;
    left->setSpacing(14);

    drop_ = new QFrame;
    drop_->setObjectName("dropZone");
    drop_->setAcceptDrops(true);
    drop_->installEventFilter(this);
    drop_->setMinimumHeight(170);
    auto* dl = new QVBoxLayout(drop_);
    dl->setAlignment(Qt::AlignCenter);
    auto* dic = new QLabel;
    dic->setPixmap(style()->standardIcon(QStyle::SP_DialogOpenButton).pixmap(36, 36));
    dic->setAlignment(Qt::AlignCenter);
    dl->addWidget(dic);
    auto* dt = new QLabel("Drop an Abaqus input file (.inp) or an ABSS results file (.abss) here");
    dt->setAlignment(Qt::AlignCenter);
    dt->setStyleSheet("font-size: 11pt; font-weight: 600;");
    dl->addWidget(dt);
    auto* orl = secondary("or", false);
    orl->setAlignment(Qt::AlignCenter);
    dl->addWidget(orl);
    auto* choose = primary("Choose file…");
    choose->setToolTip("Open an Abaqus input file (.inp) or an ABSS results file (.abss) (Ctrl+O).");
    dl->addWidget(choose, 0, Qt::AlignCenter);
    dropMsg_ = secondary({});
    dropMsg_->setAlignment(Qt::AlignCenter);
    dl->addWidget(dropMsg_);
    busy_ = secondary({});
    busy_->setAlignment(Qt::AlignCenter);
    dl->addWidget(busy_);
    left->addWidget(drop_);

    auto* demoBox = new QGroupBox("Try the demo (no Abaqus needed)");
    auto* dv = new QVBoxLayout(demoBox);
    dv->addWidget(secondary("Bundled analytic models run the complete sensitivity engine in seconds. Their results are "
                            "always labelled as synthetic demo data."));
    demo_ = new QComboBox;
    for (const auto& d : demoModels()) demo_->addItem(d.title, static_cast<int>(d.id));
    demo_->setToolTip("Choose a bundled demo model.");
    dv->addWidget(demo_);
    demoDesc_ = secondary({});
    dv->addWidget(demoDesc_);
    auto* dh = new QHBoxLayout;
    auto* openDemo = new QPushButton("Open demo model");
    openDemo->setToolTip("Load the demo model and review its parameters before running (Ctrl+Shift+D).");
    auto* runDemo = primary("Run demo and show results");
    runDemo->setToolTip("Run the demo with its default parameters (first order, validated against complete re-analyses) "
                        "and open the results.");
    dh->addWidget(openDemo);
    dh->addWidget(runDemo);
    dh->addStretch(1);
    dv->addLayout(dh);
    left->addWidget(demoBox);

    auto* recentBox = new QGroupBox("Recent files");
    auto* rv = new QVBoxLayout(recentBox);
    recent_ = new QListWidget;
    recent_->setMinimumHeight(110);
    recent_->setToolTip("Double-click to open.");
    rv->addWidget(recent_);
    left->addWidget(recentBox);
    grid->addLayout(left, 3);

    auto* ready = new QGroupBox("This computer");
    auto* rg = new QGridLayout(ready);
    rg->setHorizontalSpacing(10);
    rg->setVerticalSpacing(10);
    rg->setColumnStretch(2, 1);
    int row = 0;
    auto addRow = [&](const QString& name, QLabel*& dot, QLabel*& word, QLabel*& detail, QPushButton* btn) {
        dot = new QLabel;
        word = new QLabel;
        word->setStyleSheet("font-weight: 600;");
        detail = secondary({});
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* nm = new QLabel(name);
        auto* box = new QVBoxLayout;
        box->setSpacing(2);
        auto* top = new QHBoxLayout;
        top->setSpacing(6);
        top->addWidget(dot);
        top->addWidget(nm);
        top->addWidget(word);
        top->addStretch(1);
        if (btn) top->addWidget(btn);
        box->addLayout(top);
        box->addWidget(detail);
        rg->addLayout(box, row++, 0, 1, 3);
    };
    abqBtn_ = new QPushButton("Locate…");
    abqBtn_->setToolTip("Choose the Abaqus launcher (abaqus.bat) yourself.");
    addRow("Abaqus:", abqDot_, abqWord_, abqDetail_, abqBtn_);
    licBtn_ = new QPushButton("Check now");
    licBtn_->setEnabled(false);
    licBtn_->setToolTip("Requires Abaqus.");
    addRow("Abaqus licence:", licDot_, licWord_, licDetail_, licBtn_);
    QLabel *cdot, *cword, *cdet;
    addRow("Compute:", cdot, cword, cdet, nullptr);
    cdot->setPixmap(statusDotIcon(tokens().ok).pixmap(14, 14));
    cword->setText(par::kokkosEnabled() ? "Parallel" : "Serial");
    cdet->setText(qs(par::backendDescription()));
    QLabel *sdot, *sword, *sdet;
    addRow("Sparse solvers:", sdot, sword, sdet, nullptr);
    QStringList solvers;
    for (auto k : availableSolvers()) solvers << solverName(k);
    sdot->setPixmap(statusDotIcon(tokens().ok).pixmap(14, 14));
    sword->setText(QString::number(solvers.size()));
    sdet->setText(solvers.join(", "));
    QLabel *xdot, *xword, *xdet;
    addRow("Scratch space:", xdot, xword, xdet, nullptr);
    const QString scratch = scratchDirectory();
    QStorageInfo si(scratch);
    const double freeGb = si.bytesAvailable() / 1e9;
    xdot->setPixmap(statusDotIcon(freeGb < 10 ? tokens().warn : tokens().ok).pixmap(14, 14));
    xword->setText(freeGb < 10 ? "Low" : "OK");
    xdet->setText(QDir::toNativeSeparators(scratch) + QString(" — %1 GB free").arg(freeGb, 0, 'f', 0));
    rg->addWidget(secondary("Everything runs on this computer. ABSS never uploads models or results."), row++, 0, 1, 3);
    rg->setRowStretch(row, 1);
    ready->setMinimumWidth(360);
    grid->addWidget(ready, 2);
    v->addLayout(grid);
    v->addStretch(1);

    auto updateDesc = [this]() {
        demoDesc_->setText(demoInfo(static_cast<DemoModel>(demo_->currentData().toInt())).description);
    };
    updateDesc();
    connect(demo_, &QComboBox::currentIndexChanged, this, updateDesc);
    connect(choose, &QPushButton::clicked, this, &StartPage::chooseFileRequested);
    connect(openDemo, &QPushButton::clicked, this, [this]() { emit demoRequested(static_cast<DemoModel>(demo_->currentData().toInt()), false); });
    connect(runDemo, &QPushButton::clicked, this, [this]() { emit demoRequested(static_cast<DemoModel>(demo_->currentData().toInt()), true); });
    connect(abqBtn_, &QPushButton::clicked, this, &StartPage::locateAbaqusRequested);
    connect(licBtn_, &QPushButton::clicked, this, &StartPage::licenseCheckRequested);
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        const QString p = it->data(Qt::UserRole).toString();
        if (QFileInfo::exists(p)) emit openFileRequested(p);
    });
    refreshRecent();
    setAbaqusChecking();
    licDot_->setPixmap(statusDotIcon(tokens().disabled).pixmap(14, 14));
    licWord_->setText("Not checked");
    licDetail_->setText("Checked when you ask for it; a check briefly uses one licence token.");
}

void StartPage::setAbaqusChecking() {
    abqDot_->setPixmap(statusDotIcon(tokens().disabled).pixmap(14, 14));
    abqWord_->setText("Checking…");
    abqDetail_->setText("Looking for the Abaqus launcher (this takes a few seconds; the app stays usable).");
}

void StartPage::setAbaqus(const AbaqusInstallation& inst) {
    abaqusFound_ = inst.found;
    if (inst.found) {
        abqDot_->setPixmap(statusDotIcon(tokens().ok).pixmap(14, 14));
        abqWord_->setText("Found");
        abqDetail_->setText(qs(inst.version.empty() ? std::string("Abaqus") : inst.version) + " at " + QDir::toNativeSeparators(qs(inst.command)));
        abqBtn_->setText("Change…");
    } else {
        abqDot_->setPixmap(statusDotIcon(tokens().warn).pixmap(14, 14));
        abqWord_->setText("Not found");
        abqDetail_->setText("Needed only to run new analyses. You can still try the demo and open saved results." +
                            (inst.message.empty() ? QString() : "\n" + qs(inst.message)));
        abqBtn_->setText("Locate abaqus.bat…");
    }
    licBtn_->setEnabled(inst.found);
    licBtn_->setToolTip(inst.found ? "Run a tiny one-element job to confirm a licence token can be obtained." : "Requires Abaqus.");
}

void StartPage::setLicenseChecking() {
    licDot_->setPixmap(statusDotIcon(tokens().disabled).pixmap(14, 14));
    licWord_->setText("Checking…");
    licDetail_->setText("Running a tiny one-element job in the scratch folder…");
    licBtn_->setEnabled(false);
}

void StartPage::setLicenseResult(const AbaqusInstallation& inst) {
    licBtn_->setEnabled(abaqusFound_);
    if (!inst.licenseChecked) {
        licDot_->setPixmap(statusDotIcon(tokens().disabled).pixmap(14, 14));
        licWord_->setText("Not checked");
    } else if (inst.licenseOk) {
        licDot_->setPixmap(statusDotIcon(tokens().ok).pixmap(14, 14));
        licWord_->setText("Available");
    } else {
        licDot_->setPixmap(statusDotIcon(tokens().error).pixmap(14, 14));
        licWord_->setText("Unavailable");
    }
    licDetail_->setText(qs(inst.message));
}

void StartPage::setBusy(bool busy, const QString& what) {
    busy_->setText(busy ? what : QString());
    drop_->setEnabled(!busy);
}

void StartPage::showLoadError(const QString& t, const QString& detail) {
    error_->showError(t, detail.section('\n', 0, 0),
                      {"Check that the file opens in Abaqus/CAE (for .inp) or was saved by ABSS (for .abss)",
                       "Try the demo to confirm ABSS itself works"},
                      detail);
}

QStringList StartPage::recentFiles() { return QSettings().value("recentFiles").toStringList(); }

void StartPage::addRecent(const QString& path) {
    QStringList l = recentFiles();
    l.removeAll(path);
    l.prepend(path);
    while (l.size() > 10) l.removeLast();
    QSettings().setValue("recentFiles", l);
}

void StartPage::refreshRecent() {
    recent_->clear();
    const auto l = recentFiles();
    if (l.isEmpty()) {
        auto* it = new QListWidgetItem("Files you open will appear here.");
        it->setFlags(Qt::NoItemFlags);
        recent_->addItem(it);
        return;
    }
    for (const auto& p : l) {
        QFileInfo fi(p);
        const bool exists = fi.exists();
        auto* it = new QListWidgetItem(fi.fileName() + "   —   " + QDir::toNativeSeparators(fi.absolutePath()) +
                                       (exists ? "   ·   " + fi.lastModified().toString("yyyy-MM-dd HH:mm") : "   ·   File moved or deleted"));
        it->setData(Qt::UserRole, p);
        it->setIcon(style()->standardIcon(fi.suffix().compare("abss", Qt::CaseInsensitive) == 0 ? QStyle::SP_DialogSaveButton
                                                                                                  : QStyle::SP_FileIcon));
        if (!exists) it->setFlags(Qt::ItemIsEnabled);
        if (!exists) it->setForeground(tokens().disabled);
        recent_->addItem(it);
    }
}

bool StartPage::eventFilter(QObject* o, QEvent* e) {
    if (o == drop_) {
        if (e->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(e);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                drop_->setProperty("hover", true);
                drop_->style()->unpolish(drop_);
                drop_->style()->polish(drop_);
            }
            return true;
        }
        if (e->type() == QEvent::DragLeave || e->type() == QEvent::Drop) {
            drop_->setProperty("hover", false);
            drop_->style()->unpolish(drop_);
            drop_->style()->polish(drop_);
        }
        if (e->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(e);
            const auto urls = de->mimeData()->urls();
            if (urls.size() != 1) {
                dropMsg_->setText("Drop one .inp file at a time.");
                return true;
            }
            const QString path = urls.front().toLocalFile();
            const QString ext = QFileInfo(path).suffix().toLower();
            if (ext != "inp" && ext != "abss") {
                dropMsg_->setText("ABSS opens .inp input files and .abss results files. This file is a ." + ext + ".");
                return true;
            }
            dropMsg_->clear();
            de->acceptProposedAction();
            emit openFileRequested(path);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

// ---- ParamsPage ------------------------------------------------------------------------------------------------

ParamsPage::ParamsPage(Controller* c, QWidget* parent) : QWidget(parent), controller_(c) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 14, 16, 12);
    v->setSpacing(10);
    v->addWidget(title("Parameters"));
    v->addWidget(secondary("Choose the values whose influence you want to study. ABSS found these in the model; tick one or "
                           "more. Double-click a name to rename it."));
    lockBanner_ = lockBanner("Editing is locked while a run is in progress.");
    v->addWidget(lockBanner_);
    summary_ = secondary({});
    v->addWidget(summary_);
    diag_ = secondary({});
    diag_->hide();
    v->addWidget(diag_);

    auto* tb = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setPlaceholderText("Search parameters (name, material, set, file)");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(360);
    tb->addWidget(search_, 1);
    auto* chipRow = new QHBoxLayout;
    chipRow->setSpacing(6);
    const QStringList chipNames = {"All", "Material", "User material (UMAT)", "User element (UEL)", "Load",
                                   "Boundary condition", "Geometry", "Section", "Amplitude"};
    auto* chipGroup = new QButtonGroup(this);
    chipGroup->setExclusive(true);
    for (const auto& n : chipNames) {
        auto* b = new QToolButton;
        b->setText(n);
        b->setCheckable(true);
        b->setToolTip("Show only " + n.toLower() + " parameters.");
        chipGroup->addButton(b);
        chips_.push_back(b);
        chipRow->addWidget(b);
    }
    chips_.front()->setChecked(true);
    chips_.front()->setToolTip("Show every category.");
    showUnsupported_ = new QCheckBox("Show unsupported");
    showUnsupported_->setChecked(true);
    showUnsupported_->setToolTip("Also list values that ABSS found but cannot change safely (the reason is in the tooltip).");
    tb->addWidget(showUnsupported_);
    counter_ = new QLabel;
    counter_->setStyleSheet("font-weight: 600;");
    tb->addWidget(counter_);
    v->addLayout(tb);
    chipRow->addStretch(1);
    v->addLayout(chipRow);

    auto* split = new QSplitter(Qt::Horizontal);
    auto* tableWrap = new QWidget;
    auto* tw = new QVBoxLayout(tableWrap);
    tw->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableWidget(0, 7);
    table_->setHorizontalHeaderLabels({"", "Name", "Category", "Nominal value", "Units", "Confidence", "Source"});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tw->addWidget(table_);
    empty_ = secondary({});
    empty_->setAlignment(Qt::AlignCenter);
    empty_->hide();
    tw->addWidget(empty_);
    split->addWidget(tableWrap);

    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(8, 0, 0, 0);
    auto* infoCard = card();
    auto* iv = new QVBoxLayout(infoCard);
    info_ = new QLabel("Select a row to see what the value controls.");
    info_->setWordWrap(true);
    info_->setTextFormat(Qt::RichText);
    iv->addWidget(info_);
    snippet_ = new QLabel;
    snippet_->setFont(monospaceFont());
    snippet_->setWordWrap(true);
    snippet_->setTextFormat(Qt::RichText);
    snippet_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    iv->addWidget(snippet_);
    rv->addWidget(infoCard);
    preview_ = new VtkView;
    preview_->setMinimumHeight(220);
    rv->addWidget(preview_, 1);
    rv->addWidget(secondary("Mesh preview (undeformed). Drag to rotate, wheel to zoom.", false));
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    auto* foot = new QHBoxLayout;
    auto* nominalOnly = new QPushButton("Only compute the nominal result");
    nominalOnly->setFlat(true);
    nominalOnly->setToolTip("Continue without parameters: the run computes order 0 (the nominal analysis) only.");
    foot->addWidget(nominalOnly);
    foot->addStretch(1);
    next_ = primary("Next: Set up");
    foot->addWidget(next_);
    v->addLayout(foot);

    connect(search_, &QLineEdit::textChanged, this, &ParamsPage::applyFilter);
    connect(chipGroup, &QButtonGroup::buttonClicked, this, [this]() { applyFilter(); });
    connect(showUnsupported_, &QCheckBox::toggled, this, &ParamsPage::applyFilter);
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (loading_) return;
        const int p = table_->item(it->row(), 0)->data(Qt::UserRole).toInt();
        if (it->column() == 0) {
            updateCounter();
            emit selectionChanged();
        } else if (it->column() == 1) {
            controller_->setParameterName(p, it->text());
        }
    });
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) { showInfo(row); });
    connect(next_, &QPushButton::clicked, this, &ParamsPage::nextRequested);
    connect(nominalOnly, &QPushButton::clicked, this, [this]() {
        setSelected({});
        emit selectionChanged();
        emit nextRequested();
    });
    auto* sc = new QShortcut(QKeySequence::Find, this);
    connect(sc, &QShortcut::activated, this, &ParamsPage::focusSearch);
    updateCounter();
}

void ParamsPage::focusSearch() {
    search_->setFocus();
    search_->selectAll();
}

void ParamsPage::applyTheme() {
    preview_->scene().setTheme(tokens().dark);
    preview_->render();
}

void ParamsPage::reload() {
    loading_ = true;
    table_->setRowCount(0);
    auto m = controller_->model();
    if (!m) {
        loading_ = false;
        return;
    }
    const bool demo = controller_->source() == Controller::Source::Demo;
    table_->setRowCount(static_cast<int>(m->parameters.size()));
    for (size_t i = 0; i < m->parameters.size(); ++i) {
        const auto& p = m->parameters[i];
        const int r = static_cast<int>(i);
        auto* chk = new QTableWidgetItem;
        chk->setData(Qt::UserRole, r);
        chk->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | (p.supported ? Qt::ItemIsEnabled : Qt::NoItemFlags));
        chk->setCheckState(Qt::Unchecked);
        if (!p.supported) chk->setToolTip(p.note.empty() ? "ABSS cannot change this value safely." : qs(p.note));
        table_->setItem(r, 0, chk);
        auto* name = new QTableWidgetItem(qs(p.name));
        name->setToolTip("Original name: " + qs(p.id) + "\nDouble-click to rename.");
        name->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
        table_->setItem(r, 1, name);
        auto* cat = new QTableWidgetItem(categoryLabel(p.category));
        cat->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        table_->setItem(r, 2, cat);
        auto* nom = new QTableWidgetItem(QString::number(p.nominal, 'g', 6));
        nom->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        nom->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        table_->setItem(r, 3, nom);
        auto* units = new QTableWidgetItem(p.units.empty() ? "model units" : qs(p.units));
        units->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        units->setToolTip(p.units.empty() ? "Abaqus is not unit-aware; ABSS never invents units." : QString());
        table_->setItem(r, 4, units);
        QString conf, tip;
        QColor col;
        if (!p.supported) {
            conf = "Unsupported";
            col = tokens().error;
            tip = p.note.empty() ? "ABSS cannot change this value safely." : qs(p.note);
        } else if (p.confidence == Confidence::OpaqueIndexed) {
            conf = "Indexed";
            col = tokens().text2;
            tip = "ABSS can change this value, but cannot know what it means inside your compiled user subroutine.";
        } else {
            conf = "Known";
            col = tokens().ok;
            tip = "Recognized meaning (defined by the Abaqus keyword documentation).";
        }
        auto* ci = new QTableWidgetItem("● " + conf);
        ci->setForeground(col);
        ci->setToolTip(tip);
        ci->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        table_->setItem(r, 5, ci);
        QString src = demo ? "bundled demo model" : "—";
        if (!p.targets.empty()) {
            src = QFileInfo(qs(p.targets.front().ref.file)).fileName() + ":" + QString::number(p.targets.front().ref.line);
            if (p.targets.size() > 1) src += QString(" (+%1 more)").arg(p.targets.size() - 1);
        }
        auto* si = new QTableWidgetItem(src);
        si->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        if (!p.targets.empty()) si->setToolTip(QDir::toNativeSeparators(qs(p.targets.front().ref.file)));
        table_->setItem(r, 6, si);
        if (!p.supported)
            for (int col2 = 1; col2 < 7; ++col2) table_->item(r, col2)->setForeground(tokens().disabled);
    }
    table_->resizeColumnsToContents();
    table_->setColumnWidth(0, 32);
    if (table_->columnWidth(1) > 220) table_->setColumnWidth(1, 220);
    loading_ = false;
    if (demo) setSelected(defaultParameterIndices(controller_->demoModel(), *m));
    // summary
    std::map<std::string, int> types;
    for (const auto& e : m->mesh.elements) types[e.type]++;
    QStringList t;
    for (const auto& [k, n] : types) t << QString("%1 × %2").arg(qs(k)).arg(n);
    QString s = QString("%1 · %2 nodes · %3 elements (%4)")
                    .arg(controller_->displayName())
                    .arg(m->mesh.nodeCount())
                    .arg(m->mesh.elementCount())
                    .arg(t.join(", "));
    if (!m->frames.empty()) s += QString(" · %1 increments").arg(m->frames.size() - 1);
    if (controller_->deck()) s += QString(" · %1 step(s)").arg(controller_->deck()->steps().size());
    if (m->synthetic) s += " · synthetic demo model";
    summary_->setText(s);
    const auto& dg = controller_->deckDiagnostics();
    if (!dg.empty()) {
        QStringList l;
        for (size_t i = 0; i < dg.size() && i < 6; ++i) l << qs(dg[i]);
        if (dg.size() > 6) l << QString("… and %1 more").arg(dg.size() - 6);
        diag_->setText("Notes from reading the input file:\n" + l.join("\n"));
        diag_->show();
    } else {
        diag_->hide();
    }
    preview_->scene().setMesh(m->mesh);
    preview_->scene().setTheme(tokens().dark);
    preview_->scene().setLegend("", "", false);
    preview_->scene().setWatermark(m->synthetic ? "DEMO — synthetic model" : "");
    const bool planar = preview_->scene().isPlanar();
    preview_->scene().setParallelProjection(planar);
    preview_->scene().applyView(planar ? viz::Scene::View::XY : viz::Scene::View::Iso);
    preview_->render();
    applyFilter();
    updateCounter();
    if (table_->rowCount() > 0) table_->setCurrentCell(0, 1);
}

void ParamsPage::applyFilter() {
    const QString q = search_->text().trimmed();
    QString cat;
    for (size_t i = 1; i < chips_.size(); ++i)
        if (chips_[i]->isChecked()) cat = chips_[i]->text();
    auto m = controller_->model();
    int visible = 0;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const auto& p = m->parameters[static_cast<size_t>(table_->item(r, 0)->data(Qt::UserRole).toInt())];
        bool ok = cat.isEmpty() || table_->item(r, 2)->text() == cat;
        if (!showUnsupported_->isChecked() && !p.supported) ok = false;
        if (ok && !q.isEmpty()) {
            const QString hay = qs(p.name + " " + p.id + " " + p.description + " " + p.association) + " " + table_->item(r, 6)->text();
            ok = hay.contains(q, Qt::CaseInsensitive);
        }
        table_->setRowHidden(r, !ok);
        visible += ok ? 1 : 0;
    }
    if (table_->rowCount() == 0) {
        empty_->setText("No adjustable values were found in this model.");
        empty_->show();
    } else if (visible == 0) {
        empty_->setText(q.isEmpty() ? "No parameters in this category." : "No parameters match \"" + q + "\". Clear the search to see all.");
        empty_->show();
    } else {
        empty_->hide();
    }
}

void ParamsPage::showInfo(int row) {
    auto m = controller_->model();
    if (!m || row < 0 || row >= table_->rowCount()) return;
    const auto& p = m->parameters[static_cast<size_t>(table_->item(row, 0)->data(Qt::UserRole).toInt())];
    QString html = "<b>" + qs(p.name).toHtmlEscaped() + "</b> — " + categoryLabel(p.category) + "<br>";
    if (p.confidence == Confidence::OpaqueIndexed)
        html += "This value is passed to a user subroutine by position. ABSS does not know its meaning.<br>";
    if (!p.description.empty()) html += qs(p.description).toHtmlEscaped() + "<br>";
    if (!p.association.empty()) html += "Applies to: " + qs(p.association).toHtmlEscaped() + "<br>";
    if (!p.steps.empty()) {
        QStringList s;
        for (const auto& x : p.steps) s << qs(x);
        html += "Applies in steps: " + s.join(", ").toHtmlEscaped() + "<br>";
    }
    html += QString("Nominal value: %1 · default perturbation: %2 relative").arg(p.nominal, 0, 'g', 6).arg(p.relativeStep, 0, 'g', 2);
    if (p.mustBePositive) html += " · must stay &gt; 0";
    if (!p.supported) html += "<br><span style='color:" + tokens().error.name() + "'>Not supported: " + qs(p.note).toHtmlEscaped() + "</span>";
    info_->setText(html);
    snippet_->clear();
    const auto* deck = controller_->deck();
    if (deck && !p.targets.empty()) {
        const auto& ref = p.targets.front().ref;
        for (const auto& f : deck->files()) {
            if (f.path != ref.file) continue;
            QString s;
            for (int ln = std::max(1, ref.line - 1); ln <= std::min(static_cast<int>(f.lines.size()), ref.line + 1); ++ln) {
                QString text = qs(f.lines[static_cast<size_t>(ln - 1)]).toHtmlEscaped();
                if (ln == ref.line) text = "<span style='background:" + tokens().accent.name() + "33'>" + text + "</span>";
                s += QString("<span style='color:%1'>%2</span>  %3<br>").arg(tokens().text2.name()).arg(ln, 4).arg(text);
            }
            snippet_->setText(QFileInfo(qs(ref.file)).fileName() + "<br>" + s);
            break;
        }
    }
}

std::vector<int> ParamsPage::selected() const {
    std::vector<int> out;
    for (int r = 0; r < table_->rowCount(); ++r)
        if (table_->item(r, 0)->checkState() == Qt::Checked) out.push_back(table_->item(r, 0)->data(Qt::UserRole).toInt());
    return out;
}

void ParamsPage::setSelected(const std::vector<int>& idx) {
    loading_ = true;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const int p = table_->item(r, 0)->data(Qt::UserRole).toInt();
        const bool on = std::find(idx.begin(), idx.end(), p) != idx.end() && (table_->item(r, 0)->flags() & Qt::ItemIsEnabled);
        table_->item(r, 0)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    loading_ = false;
    updateCounter();
}

void ParamsPage::updateCounter() {
    const auto n = selected().size();
    counter_->setText(QString("%1 selected of %2").arg(n).arg(table_->rowCount()));
    next_->setEnabled(n > 0);
    next_->setToolTip(n > 0 ? "Continue to the run settings." : "Select at least one parameter to study.");
}

void ParamsPage::setLocked(bool locked) {
    lockBanner_->setVisible(locked);
    table_->setEnabled(!locked);
}

// ---- SetupPage -------------------------------------------------------------------------------------------------

SetupPage::SetupPage(Controller* c, QWidget* parent) : QWidget(parent), controller_(c) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 16, 12);
    outer->setSpacing(10);
    outer->addWidget(title("Set up"));
    lockBanner_ = lockBanner("Editing is locked while a run is in progress.");
    outer->addWidget(lockBanner_);
    auto* cols = new QHBoxLayout;
    cols->setSpacing(16);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* form = new QWidget;
    auto* v = new QVBoxLayout(form);
    v->setContentsMargins(0, 0, 8, 0);
    v->setSpacing(12);

    auto* what = new QGroupBox("What to compute");
    auto* wv = new QVBoxLayout(what);
    paramsSummary_ = secondary({});
    wv->addWidget(paramsSummary_);
    auto* oh = new QHBoxLayout;
    oh->addWidget(new QLabel("Derivative order"));
    order_ = new QButtonGroup(this);
    const char* sub[] = {"Nominal only", "Rate of change", "Curvature", "Third order (experimental)"};
    for (int k = 0; k <= 3; ++k) {
        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        auto* b = new QToolButton;
        b->setText(QString::number(k));
        b->setCheckable(true);
        b->setProperty("segment", true);
        b->setMinimumWidth(56);
        b->setAccessibleName(QString("Derivative order %1").arg(k));
        order_->addButton(b, k);
        col->addWidget(b, 0, Qt::AlignHCenter);
        auto* l = secondary(sub[k], false);
        l->setAlignment(Qt::AlignHCenter);
        if (k == 3) l->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(tokens().warn.name()));
        col->addWidget(l);
        oh->addLayout(col);
    }
    expChip_ = new QLabel("Experimental");
    styleBadge(expChip_, "warn");
    expChip_->setToolTip("Computed, but not yet confirmed by every validation test. Use with care.");
    oh->addWidget(expChip_, 0, Qt::AlignTop);
    oh->addStretch(1);
    wv->addLayout(oh);
    orderNote_ = secondary("Order 1 answers: how much does each output change when the parameter increases a little? Order 2 "
                           "tells how that rate itself changes. Selecting an order computes every lower order as well.");
    wv->addWidget(orderNote_);
    v->addWidget(what);

    auto* outs = new QGroupBox("Outputs");
    auto* ov = new QVBoxLayout(outs);
    outputsNote_ = secondary({});
    ov->addWidget(outputsNote_);
    auto* outList = new QWidget;
    outList->setObjectName("outputsList");
    new QVBoxLayout(outList);
    ov->addWidget(outList);
    auto* ob = new QHBoxLayout;
    auto* all = new QPushButton("All");
    auto* none = new QPushButton("None");
    all->setToolTip("Keep every output.");
    none->setToolTip("Keep only the outputs that are always required.");
    ob->addWidget(all);
    ob->addWidget(none);
    ob->addStretch(1);
    ov->addLayout(ob);
    v->addWidget(outs);

    auto* inc = new QGroupBox("Increments");
    auto* iv = new QVBoxLayout(inc);
    iv->addWidget(new QLabel("Every accepted increment (always; computations are never subsampled)."));
    v->addWidget(inc);

    auto* checks = new QGroupBox("Checks");
    auto* cv = new QVBoxLayout(checks);
    validate_ = new QCheckBox("Validate against full re-analyses");
    validate_->setChecked(true);
    validate_->setToolTip("After the run, re-solve the complete history at perturbed parameter values and compare. "
                          "Results that agree are marked Validated; others Failed. Costs extra complete analyses.");
    cv->addWidget(validate_);
    v->addWidget(checks);

    auto* abq = new QGroupBox("Abaqus");
    abaqusBox_ = abq;
    auto* abf = new QFormLayout(abq);
    auto* subRow = new QHBoxLayout;
    userSub_ = new QLineEdit;
    userSub_->setPlaceholderText("Not needed for this model");
    userSub_->setToolTip("UMAT/UEL used by this input file. A precompiled object or library (.obj/.o/.dll/.lib) is passed "
                         "to Abaqus as-is; Fortran/C source (.f/.for/.f90/.c/.cpp) is compiled by Abaqus, which needs a "
                         "configured Intel oneAPI Fortran + Visual Studio. ABSS never reads or uploads the subroutine.");
    auto* browse = new QPushButton("File…");
    browse->setToolTip("Fortran/C source, object file (.obj), or a prebuilt standardU.dll");
    auto* browseDir = new QPushButton("Folder…");
    browseDir->setToolTip("A folder containing a prebuilt standardU.dll (no source and no compiler needed)");
    subRow->addWidget(userSub_, 1);
    subRow->addWidget(browse);
    subRow->addWidget(browseDir);
    abf->addRow("User subroutine", subRow);
    userSubNote_ = secondary({});
    userSubNote_->setWordWrap(true);
    abf->addRow(userSubNote_);
    jobs_ = new QSpinBox;
    jobs_->setRange(1, 16);
    jobs_->setValue(std::clamp(QSettings().value("abaqusJobs", 2).toInt(), 1, 16));
    jobs_->setToolTip("How many Abaqus jobs ABSS may run at the same time. Each job takes licence tokens; a job that "
                      "cannot get a licence fails, so keep this within your licence.");
    abf->addRow("Simultaneous Abaqus jobs", jobs_);
    v->addWidget(abq);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(
            this, "Choose the user subroutine (UMAT/UEL)", QFileInfo(controller_->inpPath()).absolutePath(),
            "User subroutines (*.obj *.o *.dll *.lib *.f *.for *.f90 *.c *.cpp);;All files (*)");
        if (!f.isEmpty()) userSub_->setText(QDir::toNativeSeparators(f));
    });
    connect(browseDir, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, "Folder containing a prebuilt standardU.dll",
                                                            QFileInfo(controller_->inpPath()).absolutePath());
        if (!d.isEmpty()) userSub_->setText(QDir::toNativeSeparators(d));
    });
    connect(userSub_, &QLineEdit::textChanged, this, [this]() {
        updateCost();
        emit settingsChanged();
    });
    connect(jobs_, &QSpinBox::valueChanged, this, [this](int n) {
        QSettings().setValue("abaqusJobs", n);
        updateCost();
        emit settingsChanged();
    });

    auto* advToggle = new QToolButton;
    advToggle->setText("▸ Advanced settings");
    advToggle->setCheckable(true);
    advToggle->setAutoRaise(true);
    v->addWidget(advToggle);
    advanced_ = new QGroupBox("Advanced");
    auto* af = new QFormLayout(advanced_);
    step_ = new QDoubleSpinBox;
    step_->setDecimals(8);
    step_->setRange(0.0, 0.1);
    step_->setSingleStep(1e-4);
    step_->setSpecialValueText("Automatic (each parameter's default)");
    step_->setToolTip("Relative perturbation h/|a| used by the finite-difference stencils.");
    af->addRow("Relative step h", step_);
    tangent_ = new QCheckBox("Check the tangent at every increment");
    tangent_->setChecked(true);
    tangent_->setToolTip("Compare K·v with a finite difference of the residual (two extra evaluations per increment).");
    af->addRow(tangent_);
    advanced_->hide();
    v->addWidget(advanced_);
    v->addStretch(1);
    scroll->setWidget(form);
    cols->addWidget(scroll, 3);

    auto* costCard = card();
    auto* cc = new QVBoxLayout(costCard);
    auto* ct = new QLabel("Estimated work");
    ct->setStyleSheet("font-weight: 600; font-size: 11pt;");
    cc->addWidget(ct);
    cost_ = new QLabel;
    cost_->setWordWrap(true);
    cost_->setTextFormat(Qt::RichText);
    cc->addWidget(cost_);
    warnings_ = new QLabel;
    warnings_->setWordWrap(true);
    warnings_->setTextFormat(Qt::RichText);
    cc->addWidget(warnings_);
    cc->addWidget(secondary("This is an estimate of work, not a time promise."));
    cc->addStretch(1);
    costCard->setMinimumWidth(320);
    cols->addWidget(costCard, 2, Qt::AlignTop);
    outer->addLayout(cols, 1);

    auto* foot = new QHBoxLayout;
    foot->addStretch(1);
    next_ = primary("Next: Run");
    foot->addWidget(next_);
    outer->addLayout(foot);

    connect(order_, &QButtonGroup::idClicked, this, [this](int k) {
        if (k >= 3 && !experimentalConfirmed_) {
            const auto r = QMessageBox::question(this, "Third order is experimental",
                                                 "Third-order results are experimental: they have not passed every validation "
                                                 "test. They will be marked Experimental everywhere they appear.\n\nContinue "
                                                 "with order 3?");
            if (r != QMessageBox::Yes) {
                order_->button(2)->setChecked(true);
            } else {
                experimentalConfirmed_ = true;
            }
        }
        updateCost();
        emit settingsChanged();
    });
    connect(validate_, &QCheckBox::toggled, this, [this]() {
        updateCost();
        emit settingsChanged();
    });
    connect(tangent_, &QCheckBox::toggled, this, [this]() {
        updateCost();
        emit settingsChanged();
    });
    connect(step_, &QDoubleSpinBox::valueChanged, this, [this]() { emit settingsChanged(); });
    connect(advToggle, &QToolButton::toggled, this, [this, advToggle](bool on) {
        advanced_->setVisible(on);
        advToggle->setText(on ? "▾ Advanced settings" : "▸ Advanced settings");
    });
    connect(all, &QPushButton::clicked, this, [this]() {
        for (auto* b : outputs_)
            if (b->isEnabled()) b->setChecked(true);
    });
    connect(none, &QPushButton::clicked, this, [this]() {
        for (auto* b : outputs_)
            if (b->isEnabled()) b->setChecked(false);
    });
    connect(next_, &QPushButton::clicked, this, &SetupPage::nextRequested);
    order_->button(1)->setChecked(true);
    expChip_->hide();
}

void SetupPage::reload() {
    auto m = controller_->model();
    auto* list = findChild<QWidget*>("outputsList");
    for (auto* b : outputs_) delete b;
    outputs_.clear();
    if (!m) return;
    const bool inp = controller_->source() == Controller::Source::Inp;
    abaqusBox_->setVisible(inp);
    const auto* deck = controller_->deck();
    const bool needsSub = inp && deck && (deck->usesUserMaterial() || deck->usesUserElement());
    userSub_->setEnabled(needsSub);
    userSub_->setPlaceholderText(needsSub ? "Required: this model uses a user subroutine" : "Not needed for this model");
    if (!needsSub) userSub_->clear();
    QString tcText;
    if (needsSub) {
        const auto tc = detectCompilerToolchain();
        tcText = tc.msvc && tc.intelFortran
                     ? QString("Compilers on this computer: Microsoft C++ + Intel %1, so source and .obj files work too.")
                           .arg(qs(tc.fortranCompiler))
                     : QString("No Fortran compiler/linker on this computer: choose a prebuilt standardU.dll (or its "
                               "folder), or run scripts\\setup_dependencies.ps1 -UserSubroutines to install them.");
    }
    userSubNote_->setText(
        needsSub ? QString("This input file uses a %1. Abaqus can only run it with the compiled subroutine. A prebuilt "
                           "standardU.dll (or the folder containing it) needs no source code and no compiler; source "
                           "(.f/.for/.c) or an .obj file is built once into such a library. %2")
                       .arg(deck->usesUserElement() ? "user element (UEL)" : "user material (UMAT)", tcText)
                 : QString());
    userSubNote_->setVisible(needsSub);
    validate_->setChecked(!inp);
    validate_->setText(inp ? "Validate against full re-analyses (each is a complete Abaqus analysis)" : "Validate against full re-analyses");
    if (m->fields.empty()) {
        outputsNote_->setText("Outputs are read from the input file's *OUTPUT requests after the nominal analysis; every requested "
                              "output is kept.");
    } else {
        outputsNote_->setText("Choose which results to keep. Every selected output is differentiated at every increment.");
        for (const auto& f : m->fields) {
            auto* b = new QCheckBox(qs(f.name + " — " + f.description));
            b->setProperty("field", qs(f.name));
            b->setChecked(true);
            if (f.name == "U") {
                b->setEnabled(false);
                b->setToolTip("Always kept: needed for the deformed shape.");
            }
            connect(b, &QCheckBox::toggled, this, [this]() {
                updateCost();
                emit settingsChanged();
            });
            list->layout()->addWidget(b);
            outputs_.push_back(b);
        }
    }
    setParameters(params_);
}

void SetupPage::setParameters(const std::vector<int>& idx) {
    params_ = idx;
    auto m = controller_->model();
    QStringList names;
    if (m)
        for (int p : idx) names << qs(m->parameters.at(static_cast<size_t>(p)).name);
    paramsSummary_->setText(idx.empty() ? "No parameters selected: only the nominal result (order 0) can be computed."
                                        : "Parameters: " + names.join(", "));
    for (int k = 1; k <= 3; ++k) {
        order_->button(k)->setEnabled(!idx.empty());
        order_->button(k)->setToolTip(idx.empty() ? "Select at least one parameter on the Parameters page."
                                      : k == 3 ? "Third order: experimental — computed, but not confirmed by every validation test."
                                               : QString("Compute orders 0 to %1.").arg(k));
    }
    order_->button(0)->setToolTip("Compute the nominal analysis only.");
    if (idx.empty()) order_->button(0)->setChecked(true);
    else if (order_->checkedId() == 0) order_->button(1)->setChecked(true);
    updateCost();
}

RunSettings SetupPage::settings() const {
    RunSettings s;
    s.parameters = params_;
    s.maxOrder = std::max(0, order_->checkedId());
    if (params_.empty()) s.maxOrder = 0;
    s.validate = validate_->isChecked();
    s.tangentCheck = tangent_->isChecked();
    s.relativeStep = step_->value();
    for (auto* b : outputs_)
        if (b->isChecked()) s.outputs.push_back(b->property("field").toString().toStdString());
    s.abaqusCommand = QSettings().value("abaqusCommand").toString().toStdString();
    s.userSubroutine = userSub_->text().trimmed().toStdString();
    s.concurrentJobs = jobs_->value();
    return s;
}

void SetupPage::setOrder(int k) {
    if (auto* b = order_->button(std::clamp(k, 0, 3)); b && b->isEnabled()) {
        if (k >= 3) experimentalConfirmed_ = true;
        b->setChecked(true);
    }
    updateCost();
}

void SetupPage::setLocked(bool locked) {
    lockBanner_->setVisible(locked);
    for (auto* w : findChildren<QGroupBox*>()) w->setEnabled(!locked);
    next_->setEnabled(!locked && firstProblem().isEmpty());
}

QString SetupPage::firstProblem() const {
    if (!controller_->model()) return "Open an input file or a demo first.";
    if (controller_->source() == Controller::Source::Project)
        return "These results were opened from a file. Open the model's input file or a demo to compute new results.";
    if (controller_->source() == Controller::Source::Inp) {
        const auto* deck = controller_->deck();
        if (deck && (deck->usesUserMaterial() || deck->usesUserElement())) {
            const QString f = userSub_->text().trimmed();
            if (f.isEmpty())
                return "This model uses a user subroutine (UMAT/UEL). Choose it under Abaqus ▸ User subroutine.";
            if (!QFileInfo::exists(f)) return "The chosen user subroutine file does not exist: " + f;
        }
    }
    return {};
}

void SetupPage::updateCost() {
    expChip_->setVisible(order_->checkedId() >= 3);
    const auto s = settings();
    const auto c = controller_->estimate(s);
    QString html;
    for (const auto& l : c.lines) html += l.toHtmlEscaped() + "<br>";
    cost_->setText(html);
    QString w;
    for (const auto& x : c.warnings) w += "<span style='color:" + tokens().warn.name() + "'>⚠ " + x.toHtmlEscaped() + "</span><br>";
    warnings_->setText(w);
    warnings_->setVisible(!w.isEmpty());
    const QString prob = firstProblem();
    next_->setEnabled(prob.isEmpty());
    next_->setToolTip(prob.isEmpty() ? "Continue to the Run page." : prob);
}

// ---- RunPage ---------------------------------------------------------------------------------------------------

RunPage::RunPage(Controller* c, QWidget* parent) : QWidget(parent), controller_(c) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 14, 16, 12);
    v->setSpacing(12);
    v->addWidget(title("Run"));
    status_ = new QLabel("Ready to run");
    status_->setStyleSheet("font-size: 14pt; font-weight: 600;");
    v->addWidget(status_);
    summary_ = secondary({});
    v->addWidget(summary_);
    error_ = new ErrorPanel;
    v->addWidget(error_);
    done_ = new QFrame;
    done_->setObjectName("infoBanner");
    auto* dh = new QHBoxLayout(done_);
    dh->setContentsMargins(10, 8, 10, 8);
    auto* dl = new QLabel("Results are ready.");
    dl->setStyleSheet("font-weight: 600;");
    dh->addWidget(dl, 1);
    auto* openRes = primary("Open results");
    dh->addWidget(openRes);
    done_->hide();
    v->addWidget(done_);

    auto* bh = new QHBoxLayout;
    start_ = primary("Start run");
    start_->setProperty("big", true);
    start_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    start_->setMinimumWidth(180);
    cancel_ = new QPushButton("Cancel");
    cancel_->setProperty("big", true);
    cancel_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    cancel_->setToolTip("Stop the run (Shift+F5).");
    cancel_->hide();
    bh->addWidget(start_);
    bh->addWidget(cancel_);
    bh->addStretch(1);
    v->addLayout(bh);
    bar_ = new QProgressBar;
    bar_->setRange(0, 1000);
    bar_->setValue(0);
    bar_->setFormat("%p %");
    v->addWidget(bar_);
    stage_ = secondary({});
    v->addWidget(stage_);

    auto* lh = new QHBoxLayout;
    logToggle_ = new QToolButton;
    logToggle_->setText("▾ Hide log");
    logToggle_->setCheckable(true);
    logToggle_->setChecked(true);
    logToggle_->setAutoRaise(true);
    lh->addWidget(logToggle_);
    lh->addStretch(1);
    copyLog_ = new QPushButton("Copy");
    copyLog_->setToolTip("Copy the log to the clipboard.");
    openFolder_ = new QPushButton("Open log folder");
    openFolder_->setToolTip("Run logs are saved here when a run ends.");
    lh->addWidget(copyLog_);
    lh->addWidget(openFolder_);
    v->addLayout(lh);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setFont(monospaceFont());
    log_->setMaximumBlockCount(10000);
    v->addWidget(log_, 1);

    connect(start_, &QPushButton::clicked, this, &RunPage::startRequested);
    connect(cancel_, &QPushButton::clicked, this, [this]() {
        const auto r = QMessageBox::question(this, "Stop the run?",
                                             "Stop the run? Jobs started by ABSS will be stopped. Other Abaqus jobs on this "
                                             "computer are not affected. Results computed so far are kept.");
        if (r == QMessageBox::Yes) {
            stage_->setText("Stopping after the current step…");
            emit cancelRequested();
        }
    });
    connect(openRes, &QPushButton::clicked, this, &RunPage::openResultsRequested);
    connect(logToggle_, &QToolButton::toggled, this, [this](bool on) {
        log_->setVisible(on);
        logToggle_->setText(on ? "▾ Hide log" : "▸ Show log");
    });
    connect(copyLog_, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(log_->toPlainText()); });
    connect(openFolder_, &QPushButton::clicked, this, [this]() {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}

void RunPage::setSummary(const QString& text) { summary_->setText(text); }

void RunPage::setStartEnabled(bool on, const QString& reason) {
    start_->setEnabled(on && !controller_->running());
    start_->setToolTip(on ? "Start the run (F5)." : reason);
}

void RunPage::onStarted() {
    status_->setText("Running");
    status_->setStyleSheet("font-size: 14pt; font-weight: 600;");
    error_->hide();
    done_->hide();
    start_->setEnabled(false);
    cancel_->show();
    bar_->setValue(0);
    log_->appendPlainText(QString("— Run started %1 —").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
}

void RunPage::onProgress(const QString& stage, double f) {
    bar_->setValue(static_cast<int>(1000 * std::clamp(f, 0.0, 1.0)));
    status_->setText("Running — " + stage.section('(', 0, 0).trimmed().toLower());
    stage_->setText(stage);
}

void RunPage::onLog(const QString& line) { log_->appendPlainText(line); }

void RunPage::onFinished(bool ok, bool cancelled, const QString& message, const QString& detail) {
    cancel_->hide();
    start_->setEnabled(true);
    // Save the log so "Open log folder" shows something real.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir().mkpath(dir);
    logPath_ = dir + "/run-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt";
    QFile f(logPath_);
    if (f.open(QIODevice::WriteOnly)) f.write(log_->toPlainText().toUtf8());
    if (ok) {
        const bool anyFail = detail.contains("fail", Qt::CaseInsensitive);
        status_->setText(anyFail ? "Finished — some derivatives did not pass validation" : "Finished");
        status_->setStyleSheet(QString("font-size: 14pt; font-weight: 600; color: %1;").arg(anyFail ? tokens().warn.name() : tokens().text.name()));
        bar_->setValue(1000);
        QString text = message;
        if (!detail.trimmed().isEmpty())
            text += "\nValidation against complete re-analyses:\n" + detail.trimmed() +
                    (anyFail ? "\nFailed results stay visible but are marked Failed next to the derivative selector." : QString());
        stage_->setText(text);
        done_->show();
        return;
    }
    status_->setStyleSheet("font-size: 14pt; font-weight: 600;");
    if (cancelled) {
        status_->setText("Stopped");
        stage_->setText(message);
        done_->show();
        return;
    }
    status_->setText("Failed");
    stage_->clear();
    if (message.contains("not available in this build", Qt::CaseInsensitive)) {
        error_->showError("Running Abaqus models is not available in this build",
                          message,
                          {"Try the demo models — they run the complete sensitivity engine without Abaqus",
                           "Open a saved .abss results file to explore it",
                           "The parameter discovery and mesh view above already work with this input file"},
                          detail.isEmpty() ? message : detail);
    } else if (message.contains("not found", Qt::CaseInsensitive) && message.contains("Abaqus", Qt::CaseInsensitive)) {
        error_->showError("Abaqus is needed to run this model", message,
                          {"Locate abaqus.bat on the Start page", "Try the demo models (no Abaqus needed)"}, detail);
    } else if (message.contains("user subroutine", Qt::CaseInsensitive)) {
        error_->showError("This model needs its user subroutine", message,
                          {"Choose the UMAT/UEL file under Set up ▸ Abaqus ▸ User subroutine",
                           "A precompiled .obj/.dll needs no Fortran compiler; source files need Intel oneAPI Fortran + "
                           "Visual Studio configured for Abaqus",
                           "The demo models run without Abaqus or subroutines"},
                          detail);
    } else if (message.contains("not supported", Qt::CaseInsensitive) || message.contains("not replayed", Qt::CaseInsensitive) ||
               message.contains("needs a flat input", Qt::CaseInsensitive)) {
        error_->showError("This model uses a feature ABSS cannot differentiate yet", message,
                          {"See docs/KNOWN_LIMITATIONS.md for the supported Abaqus features",
                           "Parameters and the mesh can still be inspected on the Parameters page"},
                          detail);
    } else if (message.contains("did not complete", Qt::CaseInsensitive) || message.contains("Abaqus job", Qt::CaseInsensitive)) {
        error_->showError("An Abaqus job failed", message,
                          {"Show details below: the Abaqus error message is included",
                           "The job's .dat/.msg files are in the scratch folder named in the log",
                           "If it is a licence error, lower Set up ▸ Abaqus ▸ Simultaneous Abaqus jobs"},
                          detail);
    } else {
        error_->showError("The run could not be completed", message, {"Show details below and check the log"}, detail);
    }
}

}  // namespace abss::gui
