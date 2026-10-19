// ABSS visualization: color maps (VTK-free so they can be unit tested headless).
//
// Built-in tables are generated in code, not downloaded:
//   Viridis, Plasma, Inferno  - polynomial fits of the matplotlib maps (fits by M. Zucker, CC0; maps CC0)
//   Turbo                     - Google's polynomial approximation (Apache-2.0)
//   Cool-warm                 - Moreland's diverging Msh interpolation (published algorithm)
//   Seismic, Blue-white-red   - matplotlib "seismic" segments / ColorBrewer RdBu (reversed) stops
//   Grayscale, Rainbow, Jet   - simple linear stops
#pragma once

#include <array>
#include <string>
#include <vector>

namespace abss::viz {

struct Rgb {
    double r = 0, g = 0, b = 0;  ///< 0..1
};

struct Colormap {
    std::string name;
    bool diverging = false;  ///< has a neutral midpoint intended for data centered at zero
    bool custom = false;
    std::vector<Rgb> table;  ///< evenly spaced samples from t = 0 to t = 1 (>= 2 entries)

    /// Linear interpolation in the table at t in [0, 1] (clamped).
    Rgb sample(double t) const;
};

/// Built-in maps in menu order. The first entry ("Viridis") is the default for unsigned quantities,
/// "Cool-warm" the default for signed quantities and every sensitivity.
const std::vector<Colormap>& builtinColormaps();
/// Built-in or registered custom map by name; nullptr when unknown.
const Colormap* findColormap(const std::string& name);
/// Add (or replace by name) a user-defined map for this process.
void registerCustomColormap(const Colormap& map);
const std::vector<Colormap>& customColormaps();

/// Parse a user palette: one color stop per line, either "r,g,b" or "t,r,g,b" (comma/space/semicolon separated).
/// Components in 0..1, or 0..255 when any component exceeds 1. '#' starts a comment; "#RRGGBB" hex lines are
/// also accepted. Stops with explicit t are resampled to an even 256-entry table. Throws std::invalid_argument.
Colormap parseColormapText(const std::string& text, const std::string& name);

inline constexpr const char* kDefaultUnsignedMap = "Viridis";
inline constexpr const char* kDefaultSignedMap = "Cool-warm";

}  // namespace abss::viz
