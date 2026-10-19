// ABSS GUI: style tokens (Maya UX spec §8), theme application and runtime-painted icons.
#pragma once

#include "abss/viz/colormap.hpp"

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;
class QLabel;
class QWidget;

namespace abss::gui {

enum class ThemeMode { System, Light, Dark };

struct Tokens {
    QColor window, panel, border, text, text2, disabled, accent, accentText, ok, warn, error, nodata, edges;
    QColor bannerBg, bannerText;
    bool dark = false;
};

const Tokens& tokens();
ThemeMode themeMode();
/// Apply palette + style sheet to the application (Fusion style). Emits nothing; widgets repaint.
void applyTheme(QApplication& app, ThemeMode mode);
bool systemPrefersDark();

/// Categorical colors for multi-pick series (8, spec §8.4), lightened in dark mode.
QColor categoricalColor(int i);
viz::Rgb toRgb(const QColor& c);

// Painted icons (no external files).
QIcon statusDotIcon(const QColor& c);
QIcon colormapSwatch(const viz::Colormap& map, int w = 64, int h = 12);
enum class ViewIcon { Fit, Iso, XY, XZ, YZ, Perspective, Camera };
QIcon viewIcon(ViewIcon which);
QIcon appIcon();
enum class MediaIcon { First, Prev, Play, Pause, Next, Last };
QIcon mediaIcon(MediaIcon which);

/// Badge label styling: kind = "ok", "warn", "error", "neutral", "accent", "demo".
void styleBadge(QLabel* label, const QString& kind);
/// Monospace font for logs/paths.
QFont monospaceFont();

}  // namespace abss::gui
