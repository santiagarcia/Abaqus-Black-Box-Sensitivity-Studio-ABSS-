#include "abss/gui/theme.hpp"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QImage>

#include <functional>
#include <QGuiApplication>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace abss::gui {

namespace {
Tokens makeTokens(bool dark) {
    Tokens t;
    t.dark = dark;
    if (!dark) {
        t.window = QColor("#F6F7F9");
        t.panel = QColor("#FFFFFF");
        t.border = QColor("#D9DCE1");
        t.text = QColor("#1C1E21");
        t.text2 = QColor("#5B616B");
        t.disabled = QColor("#8A9099");
        t.accent = QColor("#1F5FBF");
        t.accentText = QColor("#FFFFFF");
        t.ok = QColor("#2A7A2A");
        t.warn = QColor("#A15C00");
        t.error = QColor("#B03030");
        t.nodata = QColor("#C9CCD1");
        t.edges = QColor("#30343A");
        t.bannerBg = QColor("#FFF4E0");
        t.bannerText = QColor("#7A4500");
    } else {
        t.window = QColor("#1E2126");
        t.panel = QColor("#262A30");
        t.border = QColor("#3A3F47");
        t.text = QColor("#E6E8EB");
        t.text2 = QColor("#A0A6AF");
        t.disabled = QColor("#7A808A");
        t.accent = QColor("#5B9BFF");
        t.accentText = QColor("#1E2126");
        t.ok = QColor("#5CB85C");
        t.warn = QColor("#E0A040");
        t.error = QColor("#F06A6A");
        t.nodata = QColor("#5A5F66");
        t.edges = QColor("#0E1013");
        t.bannerBg = QColor("#3A2E1A");
        t.bannerText = QColor("#F2C27A");
    }
    return t;
}

Tokens g_tokens = makeTokens(false);
ThemeMode g_mode = ThemeMode::System;

/// Paint check box / radio indicators for the current tokens (style sheets need image files).
QString indicatorDir(const Tokens& t) {
    const QString dir = QDir::tempPath() + QString("/abss-theme-%1").arg(t.dark ? "dark" : "light");
    QDir().mkpath(dir);
    auto save = [&](const QString& name, const std::function<void(QPainter&)>& paint) {
        QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        paint(p);
        p.end();
        img.save(dir + "/" + name + ".png");
    };
    for (int dis = 0; dis < 2; ++dis) {
        const QColor border = dis ? t.border : t.text2;
        const QColor fill = dis ? t.disabled : t.accent;
        const QString suf = dis ? "_dis" : "";
        save("cb_off" + suf, [&](QPainter& p) {
            p.setPen(QPen(border, 2.5));
            p.setBrush(dis ? t.window : t.panel);
            p.drawRoundedRect(QRectF(3, 3, 26, 26), 6, 6);
        });
        save("cb_on" + suf, [&](QPainter& p) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(2, 2, 28, 28), 6, 6);
            p.setPen(QPen(t.dark ? QColor("#1E2126") : QColor(Qt::white), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF({QPointF(8.5, 16.5), QPointF(13.5, 21.5), QPointF(23.5, 10.5)}));
        });
        save("rb_off" + suf, [&](QPainter& p) {
            p.setPen(QPen(border, 2.5));
            p.setBrush(dis ? t.window : t.panel);
            p.drawEllipse(QRectF(3, 3, 26, 26));
        });
        save("rb_on" + suf, [&](QPainter& p) {
            p.setPen(QPen(fill, 2.5));
            p.setBrush(dis ? t.window : t.panel);
            p.drawEllipse(QRectF(3, 3, 26, 26));
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(QRectF(9.5, 9.5, 13, 13));
        });
    }
    return dir;
}

QString styleSheet(const Tokens& t) {
    QString qss = R"(
QWidget { font-size: 10pt; }
QMainWindow, QDialog { background: @window; }
QToolTip { color: @text; background: @panel; border: 1px solid @border; padding: 4px; }
QFrame#card, QGroupBox { background: @panel; border: 1px solid @border; border-radius: 6px; }
QGroupBox { margin-top: 14px; padding: 12px 8px 8px 8px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @text; }
QLabel#pageTitle { font-size: 15pt; font-weight: 600; color: @text; }
QLabel#secondary, QLabel[secondary="true"] { color: @text2; font-size: 9pt; }
QLabel#question { font-size: 11pt; color: @text2; }
QFrame#demoBanner { background: @bannerBg; border: 1px solid @warn; border-radius: 4px; }
QFrame#demoBanner QLabel { color: @bannerText; font-weight: 600; }
QFrame#infoBanner { background: @panel; border: 1px solid @border; border-left: 3px solid @accent; border-radius: 4px; }
QFrame#errorPanel { background: @panel; border: 1px solid @error; border-left: 4px solid @error; border-radius: 4px; }
QFrame#dropZone { background: @panel; border: 2px dashed @border; border-radius: 8px; }
QFrame#dropZone[hover="true"] { border-color: @accent; }
QFrame#whatBar { background: @panel; border-bottom: 1px solid @border; }
QListWidget#rail { background: @panel; border: none; border-right: 1px solid @border; outline: 0; padding-top: 8px; }
QListWidget#rail::item { padding: 10px 8px; margin: 2px 6px; border-radius: 6px; color: @text; }
QListWidget#rail::item:selected { background: @accent; color: @accentText; }
QListWidget#rail::item:disabled { color: @disabled; }
QPushButton, QToolButton { min-height: 26px; padding: 2px 10px; border: 1px solid @border; border-radius: 6px; background: @panel; color: @text; }
QPushButton:hover, QToolButton:hover { border-color: @accent; }
QPushButton:disabled, QToolButton:disabled { color: @disabled; background: @window; }
QPushButton[primary="true"] { background: @accent; color: @accentText; border: 1px solid @accent; font-weight: 600; min-height: 30px; padding: 2px 16px; }
QPushButton[primary="true"]:disabled { background: @border; color: @disabled; border-color: @border; }
QPushButton[big="true"] { min-height: 38px; font-size: 11pt; }
QToolButton:checked { background: @accent; color: @accentText; border-color: @accent; }
QToolButton[segment="true"] { min-width: 34px; border-radius: 4px; }
QLineEdit { min-height: 26px; border: 1px solid @border; border-radius: 4px; padding: 0 6px; background: @panel; color: @text; }
QLineEdit:focus { border-color: @accent; }
QLineEdit:disabled { color: @disabled; background: @window; }
QComboBox, QDoubleSpinBox, QSpinBox { min-height: 26px; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator, QTableView::indicator, QTreeView::indicator, QListView::indicator, QRadioButton::indicator { width: 16px; height: 16px; }
QCheckBox::indicator:unchecked, QTableView::indicator:unchecked, QTreeView::indicator:unchecked { image: url(@ind/cb_off.png); }
QCheckBox::indicator:checked, QTableView::indicator:checked, QTreeView::indicator:checked { image: url(@ind/cb_on.png); }
QCheckBox::indicator:unchecked:disabled, QTableView::indicator:unchecked:disabled { image: url(@ind/cb_off_dis.png); }
QCheckBox::indicator:checked:disabled, QTableView::indicator:checked:disabled { image: url(@ind/cb_on_dis.png); }
QRadioButton::indicator:unchecked { image: url(@ind/rb_off.png); }
QRadioButton::indicator:checked { image: url(@ind/rb_on.png); }
QRadioButton::indicator:unchecked:disabled { image: url(@ind/rb_off_dis.png); }
QRadioButton::indicator:checked:disabled { image: url(@ind/rb_on_dis.png); }
QTableView, QTreeView, QListView, QPlainTextEdit { background: @panel; border: 1px solid @border; border-radius: 4px; color: @text; alternate-background-color: @window; }
QHeaderView::section { background: @window; color: @text2; border: none; border-bottom: 1px solid @border; padding: 4px 6px; font-weight: 600; }
QProgressBar { border: 1px solid @border; border-radius: 4px; text-align: center; background: @window; min-height: 22px; color: @text; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }
QDockWidget { color: @text; font-weight: 600; }
QDockWidget::title { background: @window; padding: 6px 8px; border-bottom: 1px solid @border; }
QStatusBar { background: @panel; border-top: 1px solid @border; color: @text2; }
QStatusBar QLabel { color: @text2; padding: 0 6px; }
QLabel[badge="ok"] { color: @ok; border: 1px solid @ok; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QLabel[badge="warn"] { color: @warn; border: 1px solid @warn; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QLabel[badge="error"] { color: @error; border: 1px solid @error; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QLabel[badge="neutral"] { color: @text2; border: 1px solid @border; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QLabel[badge="accent"] { color: @accent; border: 1px solid @accent; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QLabel[badge="demo"] { color: @bannerText; background: @bannerBg; border: 1px solid @warn; border-radius: 4px; padding: 1px 6px; font-weight: 600; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal { background: @accent; width: 14px; height: 14px; margin: -6px 0; border-radius: 7px; }
)";
    auto name = [](const QColor& c) { return c.name(QColor::HexRgb); };
    qss.replace("@ind", indicatorDir(t));
    qss.replace("@window", name(t.window))
        .replace("@panel", name(t.panel))
        .replace("@border", name(t.border))
        .replace("@text2", name(t.text2))
        .replace("@text", name(t.text))
        .replace("@disabled", name(t.disabled))
        .replace("@accentText", name(t.accentText))
        .replace("@accent", name(t.accent))
        .replace("@ok", name(t.ok))
        .replace("@warn", name(t.warn))
        .replace("@error", name(t.error))
        .replace("@bannerBg", name(t.bannerBg))
        .replace("@bannerText", name(t.bannerText));
    return qss;
}
}  // namespace

const Tokens& tokens() { return g_tokens; }
ThemeMode themeMode() { return g_mode; }

bool systemPrefersDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto* h = QGuiApplication::styleHints()) return h->colorScheme() == Qt::ColorScheme::Dark;
#endif
    return false;
}

void applyTheme(QApplication& app, ThemeMode mode) {
    g_mode = mode;
    const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemPrefersDark());
    g_tokens = makeTokens(dark);
    const auto& t = g_tokens;
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.panel);
    p.setColor(QPalette::AlternateBase, t.window);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accentText);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.text2);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.border.darker(130));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    app.setPalette(p);
    app.setStyleSheet(styleSheet(t));
}

QColor categoricalColor(int i) {
    static const char* c[] = {"#1F5FBF", "#D9730D", "#2A9D8F", "#B03060", "#6A4C93", "#8C6D1F", "#3B8EA5", "#5B616B"};
    QColor col(c[((i % 8) + 8) % 8]);
    return tokens().dark ? col.lighter(140) : col;
}

viz::Rgb toRgb(const QColor& c) { return {c.redF(), c.greenF(), c.blueF()}; }

QIcon statusDotIcon(const QColor& c) {
    QPixmap pm(28, 28);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(6, 6, 16, 16));
    return QIcon(pm);
}

QIcon colormapSwatch(const viz::Colormap& map, int w, int h) {
    QPixmap pm(w, h);
    QPainter p(&pm);
    for (int x = 0; x < w; ++x) {
        const auto c = map.sample(w > 1 ? double(x) / (w - 1) : 0.0);
        p.setPen(QColor::fromRgbF(float(c.r), float(c.g), float(c.b)));
        p.drawLine(x, 0, x, h - 1);
    }
    p.setPen(QColor(0, 0, 0, 60));
    p.drawRect(0, 0, w - 1, h - 1);
    return QIcon(pm);
}

QIcon viewIcon(ViewIcon which) {
    const int s = 40;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fg = tokens().text, ac = tokens().accent;
    QPen pen(fg, 2.2);
    p.setPen(pen);
    // isometric cube corners
    const QPointF c(20, 21);
    const double a = 11;
    const QPointF top(c.x(), c.y() - a), ur(c.x() + a * 0.87, c.y() - a / 2), lr(c.x() + a * 0.87, c.y() + a / 2),
        bot(c.x(), c.y() + a), ll(c.x() - a * 0.87, c.y() + a / 2), ul(c.x() - a * 0.87, c.y() - a / 2);
    auto cube = [&](int face) {
        QPolygonF faces[3] = {QPolygonF({top, ur, c, ul}), QPolygonF({c, ur, lr, bot}), QPolygonF({ul, c, bot, ll})};
        if (face >= 0) {
            p.setBrush(QColor(ac.red(), ac.green(), ac.blue(), 150));
            p.setPen(Qt::NoPen);
            p.drawPolygon(faces[face]);
            p.setPen(pen);
        }
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({top, ur, lr, bot, ll, ul}));
        p.drawLine(c, top);
        p.drawLine(c, lr);
        p.drawLine(c, ll);
        p.drawLine(c, ur);
        p.drawLine(c, ul);
        p.drawLine(c, bot);
    };
    switch (which) {
        case ViewIcon::Fit: {
            p.drawRect(QRectF(12, 12, 16, 16));
            auto arrow = [&](QPointF from, QPointF to) {
                p.drawLine(from, to);
            };
            arrow({4, 4}, {11, 11});
            arrow({36, 4}, {29, 11});
            arrow({4, 36}, {11, 29});
            arrow({36, 36}, {29, 29});
            p.drawLine(QPointF(4, 4), QPointF(9, 4));
            p.drawLine(QPointF(4, 4), QPointF(4, 9));
            p.drawLine(QPointF(36, 4), QPointF(31, 4));
            p.drawLine(QPointF(36, 4), QPointF(36, 9));
            p.drawLine(QPointF(4, 36), QPointF(9, 36));
            p.drawLine(QPointF(4, 36), QPointF(4, 31));
            p.drawLine(QPointF(36, 36), QPointF(31, 36));
            p.drawLine(QPointF(36, 36), QPointF(36, 31));
            break;
        }
        case ViewIcon::Iso: cube(-1); p.setBrush(QColor(ac.red(), ac.green(), ac.blue(), 90)); p.setPen(Qt::NoPen);
            p.drawPolygon(QPolygonF({top, ur, lr, bot, ll, ul})); break;
        case ViewIcon::XY: cube(1); break;
        case ViewIcon::XZ: cube(0); break;
        case ViewIcon::YZ: cube(2); break;
        case ViewIcon::Perspective: {
            p.drawLine(QPointF(6, 30), QPointF(34, 30));
            p.drawLine(QPointF(6, 30), QPointF(16, 10));
            p.drawLine(QPointF(34, 30), QPointF(24, 10));
            p.drawLine(QPointF(16, 10), QPointF(24, 10));
            break;
        }
        case ViewIcon::Camera: {
            p.drawRoundedRect(QRectF(6, 12, 28, 19), 3, 3);
            p.drawEllipse(QPointF(20, 21.5), 5.5, 5.5);
            p.drawRect(QRectF(14, 8, 9, 4));
            break;
        }
    }
    return QIcon(pm);
}

QIcon appIcon() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, 64, 64);
    g.setColorAt(0, QColor("#1F5FBF"));
    g.setColorAt(1, QColor("#2A9D8F"));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);
    p.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap));
    QPainterPath path;
    path.moveTo(12, 46);
    path.cubicTo(24, 46, 26, 16, 38, 16);
    path.lineTo(52, 16);
    p.drawPath(path);
    p.setPen(QPen(QColor(255, 255, 255, 170), 3, Qt::DashLine, Qt::RoundCap));
    p.drawLine(QPointF(12, 52), QPointF(52, 22));
    return QIcon(pm);
}

void styleBadge(QLabel* label, const QString& kind) {
    const auto& t = tokens();
    QColor fg = t.text2, border = t.border, bg = Qt::transparent;
    if (kind == "ok") fg = border = t.ok;
    else if (kind == "warn") fg = border = t.warn;
    else if (kind == "error") fg = border = t.error;
    else if (kind == "accent") fg = border = t.accent;
    else if (kind == "demo") {
        fg = t.bannerText;
        border = t.warn;
        bg = t.bannerBg;
    }
    label->setProperty("badgeKind", kind);
    label->setStyleSheet(QString("QLabel { color: %1; border: 1px solid %2; background: %3; border-radius: 4px; "
                                 "padding: 2px 8px; font-weight: 600; }")
                             .arg(fg.name(), border.name(), bg.alpha() == 0 ? QString("transparent") : bg.name()));
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    label->updateGeometry();
}

QIcon mediaIcon(MediaIcon which) {
    const int s = 32;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(tokens().text);
    auto tri = [&](double x0, double x1, bool right) {
        if (right) p.drawPolygon(QPolygonF({QPointF(x0, 8), QPointF(x1, 16), QPointF(x0, 24)}));
        else p.drawPolygon(QPolygonF({QPointF(x1, 8), QPointF(x0, 16), QPointF(x1, 24)}));
    };
    switch (which) {
        case MediaIcon::First:
            p.drawRect(QRectF(7, 8, 3, 16));
            tri(10, 18, false);
            tri(17, 25, false);
            break;
        case MediaIcon::Prev: tri(10, 22, false); break;
        case MediaIcon::Play: tri(10, 24, true); break;
        case MediaIcon::Pause:
            p.drawRect(QRectF(10, 8, 4, 16));
            p.drawRect(QRectF(18, 8, 4, 16));
            break;
        case MediaIcon::Next: tri(10, 22, true); break;
        case MediaIcon::Last:
            tri(7, 15, true);
            tri(14, 22, true);
            p.drawRect(QRectF(22, 8, 3, 16));
            break;
    }
    return QIcon(pm);
}

QFont monospaceFont() {
    QFont f("Cascadia Mono");
    if (!QFontDatabase::families().contains("Cascadia Mono")) {
        f = QFont("Consolas");
        if (!QFontDatabase::families().contains("Consolas")) f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    f.setPointSize(9);
    return f;
}

}  // namespace abss::gui
