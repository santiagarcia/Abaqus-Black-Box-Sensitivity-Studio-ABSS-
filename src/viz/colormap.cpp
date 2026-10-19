#include "abss/viz/colormap.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace abss::viz {

namespace {

constexpr int kN = 256;
constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

using Poly = std::array<std::array<double, 3>, 7>;

Colormap fromPoly(const std::string& name, const Poly& c) {
    Colormap m;
    m.name = name;
    for (int i = 0; i < kN; ++i) {
        const double t = double(i) / (kN - 1);
        double v[3];
        for (int ch = 0; ch < 3; ++ch) {
            double acc = c[6][ch];
            for (int k = 5; k >= 0; --k) acc = c[static_cast<size_t>(k)][ch] + t * acc;
            v[ch] = clamp01(acc);
        }
        m.table.push_back({v[0], v[1], v[2]});
    }
    return m;
}

Colormap fromStops(const std::string& name, const std::vector<std::pair<double, Rgb>>& stops, bool diverging) {
    Colormap m;
    m.name = name;
    m.diverging = diverging;
    for (int i = 0; i < kN; ++i) {
        const double t = double(i) / (kN - 1);
        size_t k = 1;
        while (k + 1 < stops.size() && stops[k].first < t) ++k;
        const auto& [t0, c0] = stops[k - 1];
        const auto& [t1, c1] = stops[k];
        const double u = t1 > t0 ? clamp01((t - t0) / (t1 - t0)) : 0.0;
        m.table.push_back({c0.r + u * (c1.r - c0.r), c0.g + u * (c1.g - c0.g), c0.b + u * (c1.b - c0.b)});
    }
    return m;
}

Rgb hex(unsigned v) { return {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0}; }

Colormap turbo() {
    // Google "Turbo" polynomial approximation (Apache-2.0).
    Colormap m;
    m.name = "Turbo";
    const double kr4[4] = {0.13572138, 4.61539260, -42.66032258, 132.13108234};
    const double kg4[4] = {0.09140261, 2.19418839, 4.84296658, -14.18503333};
    const double kb4[4] = {0.10667330, 12.64194608, -60.58204836, 110.36276771};
    const double kr2[2] = {-152.94239396, 59.28637943};
    const double kg2[2] = {4.27729857, 2.82956604};
    const double kb2[2] = {-89.90310912, 27.34824973};
    for (int i = 0; i < kN; ++i) {
        const double x = double(i) / (kN - 1);
        const double v4[4] = {1, x, x * x, x * x * x};
        const double v2[2] = {v4[2] * v4[2], v4[3] * v4[2]};
        auto ch = [&](const double* a4, const double* a2) {
            return clamp01(a4[0] * v4[0] + a4[1] * v4[1] + a4[2] * v4[2] + a4[3] * v4[3] + a2[0] * v2[0] + a2[1] * v2[1]);
        };
        m.table.push_back({ch(kr4, kr2), ch(kg4, kg2), ch(kb4, kb2)});
    }
    return m;
}

// ---- Moreland diverging (Msh) -----------------------------------------------------------------------------------
struct Msh {
    double M, s, h;
};
double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double toSrgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }
constexpr double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;
double fLab(double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; }
double fLabInv(double t) { return t > 0.206893 ? t * t * t : (t - 16.0 / 116.0) / 7.787; }

Msh rgbToMsh(Rgb c) {
    const double r = toLinear(c.r), g = toLinear(c.g), b = toLinear(c.b);
    const double X = 0.4124 * r + 0.3576 * g + 0.1805 * b;
    const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
    const double L = 116 * fLab(Y / Yn) - 16;
    const double A = 500 * (fLab(X / Xn) - fLab(Y / Yn));
    const double B = 200 * (fLab(Y / Yn) - fLab(Z / Zn));
    const double M = std::sqrt(L * L + A * A + B * B);
    return {M, M > 0 ? std::acos(L / M) : 0.0, std::atan2(B, A)};
}

Rgb mshToRgb(Msh m) {
    const double L = m.M * std::cos(m.s), A = m.M * std::sin(m.s) * std::cos(m.h), B = m.M * std::sin(m.s) * std::sin(m.h);
    const double fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
    const double X = Xn * fLabInv(fx), Y = Yn * fLabInv(fy), Z = Zn * fLabInv(fz);
    const double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    const double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    const double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    return {clamp01(toSrgb(std::max(r, 0.0))), clamp01(toSrgb(std::max(g, 0.0))), clamp01(toSrgb(std::max(b, 0.0)))};
}

double adjustHue(Msh m, double Munsat) {
    if (m.M >= Munsat) return m.h;
    const double hSpin = m.s * std::sqrt(Munsat * Munsat - m.M * m.M) / (m.M * std::sin(m.s));
    return m.h > -kPi / 3 ? m.h + hSpin : m.h - hSpin;
}

Rgb diverging(Rgb c1, Rgb c2, double t) {
    Msh a = rgbToMsh(c1), b = rgbToMsh(c2);
    if (a.s > 0.05 && b.s > 0.05 && std::abs(a.h - b.h) > kPi / 3) {
        const double Mmid = std::max({a.M, b.M, 88.0});
        if (t < 0.5) {
            b = {Mmid, 0, 0};
            t = 2 * t;
        } else {
            a = {Mmid, 0, 0};
            t = 2 * t - 1;
        }
    }
    if (a.s < 0.05 && b.s > 0.05) a.h = adjustHue(b, a.M);
    else if (b.s < 0.05 && a.s > 0.05) b.h = adjustHue(a, b.M);
    return mshToRgb({(1 - t) * a.M + t * b.M, (1 - t) * a.s + t * b.s, (1 - t) * a.h + t * b.h});
}

Colormap coolWarm() {
    Colormap m;
    m.name = "Cool-warm";
    m.diverging = true;
    const Rgb c1{59 / 255.0, 76 / 255.0, 192 / 255.0}, c2{180 / 255.0, 4 / 255.0, 38 / 255.0};
    for (int i = 0; i < kN; ++i) m.table.push_back(diverging(c1, c2, double(i) / (kN - 1)));
    return m;
}

std::vector<Colormap> makeBuiltins() {
    std::vector<Colormap> v;
    v.push_back(fromPoly("Viridis", {{{0.2777273272234177, 0.005407344544966578, 0.3340998053353061},
                                      {0.1050930431085774, 1.404613529898575, 1.384590162594685},
                                      {-0.3308618287255563, 0.214847559468213, 0.09509516302823659},
                                      {-4.634230498983486, -5.799100973351585, -19.33244095627987},
                                      {6.228269936347081, 14.17993336680509, 56.69055260068105},
                                      {4.776384997670288, -13.74514537774601, -65.35303263337234},
                                      {-5.435455855934631, 4.645852612178535, 26.3124352495832}}}));
    v.push_back(fromPoly("Plasma", {{{0.05873234392399702, 0.02333670892565664, 0.5433401826748754},
                                     {2.176514634195958, 0.2383834171260182, 0.7539604599784036},
                                     {-2.689460476458034, -7.455851135738909, 3.110799939717086},
                                     {6.130348345893603, 42.3461881477227, -28.51885465332158},
                                     {-11.10743619062271, -82.66631109428045, 60.13984767418263},
                                     {10.02306557647065, 71.41361770095349, -54.07218655560067},
                                     {-3.658713842777788, -22.93153465461149, 18.19190778539828}}}));
    v.push_back(fromPoly("Inferno", {{{0.0002189403691192265, 0.001651004631001012, -0.01948089843709184},
                                      {0.1065134194856116, 0.5639564367884091, 3.932712388889277},
                                      {11.60249308247187, -3.972853965665698, -15.9423941062914},
                                      {-41.70399613139459, 17.43639888205313, 44.35414519872813},
                                      {77.162935699427, -33.40235894210092, -81.80730925738993},
                                      {-71.31942824499214, 32.62606426397723, 73.20951985803202},
                                      {25.13112622477341, -12.24266895238567, -23.07032500287172}}}));
    v.push_back(turbo());
    v.push_back(coolWarm());
    v.push_back(fromStops("Seismic", {{0.0, {0, 0, 0.3}}, {0.25, {0, 0, 1}}, {0.5, {1, 1, 1}}, {0.75, {1, 0, 0}}, {1.0, {0.5, 0, 0}}}, true));
    {
        const unsigned rdbu[11] = {0x053061, 0x2166ac, 0x4393c3, 0x92c5de, 0xd1e5f0, 0xf7f7f7,
                                   0xfddbc7, 0xf4a582, 0xd6604d, 0xb2182b, 0x67001f};
        std::vector<std::pair<double, Rgb>> s;
        for (int i = 0; i < 11; ++i) s.push_back({i / 10.0, hex(rdbu[i])});
        v.push_back(fromStops("Blue-white-red", s, true));
    }
    v.push_back(fromStops("Grayscale", {{0.0, {0.05, 0.05, 0.05}}, {1.0, {0.97, 0.97, 0.97}}}, false));
    v.push_back(fromStops("Rainbow", {{0.0, {0, 0, 1}}, {0.25, {0, 1, 1}}, {0.5, {0, 1, 0}}, {0.75, {1, 1, 0}}, {1.0, {1, 0, 0}}}, false));
    v.push_back(fromStops("Jet", {{0.0, {0, 0, 0.5}}, {0.125, {0, 0, 1}}, {0.375, {0, 1, 1}}, {0.625, {1, 1, 0}}, {0.875, {1, 0, 0}}, {1.0, {0.5, 0, 0}}}, false));
    return v;
}

std::mutex& customMutex() {
    static std::mutex m;
    return m;
}
std::vector<Colormap>& customStore() {
    static std::vector<Colormap> v;
    return v;
}

}  // namespace

Rgb Colormap::sample(double t) const {
    if (table.empty()) return {0.5, 0.5, 0.5};
    if (!(t == t)) t = 0;  // NaN
    t = clamp01(t);
    const double x = t * double(table.size() - 1);
    const size_t i = std::min(static_cast<size_t>(x), table.size() - 1);
    const size_t j = std::min(i + 1, table.size() - 1);
    const double u = x - double(i);
    return {table[i].r + u * (table[j].r - table[i].r), table[i].g + u * (table[j].g - table[i].g),
            table[i].b + u * (table[j].b - table[i].b)};
}

const std::vector<Colormap>& builtinColormaps() {
    static const std::vector<Colormap> v = makeBuiltins();
    return v;
}

const Colormap* findColormap(const std::string& name) {
    for (const auto& m : builtinColormaps())
        if (m.name == name) return &m;
    std::lock_guard<std::mutex> lk(customMutex());
    for (const auto& m : customStore())
        if (m.name == name) return &m;
    return nullptr;
}

void registerCustomColormap(const Colormap& map) {
    std::lock_guard<std::mutex> lk(customMutex());
    auto& v = customStore();
    for (auto& m : v)
        if (m.name == map.name) {
            m = map;
            m.custom = true;
            return;
        }
    v.push_back(map);
    v.back().custom = true;
}

const std::vector<Colormap>& customColormaps() { return customStore(); }

Colormap parseColormapText(const std::string& text, const std::string& name) {
    std::vector<std::vector<double>> rows;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        std::string s = line.substr(first);
        if (s[0] == '#' && s.size() >= 7 && std::isxdigit(static_cast<unsigned char>(s[1]))) {
            bool hexOk = true;
            for (int k = 1; k <= 6; ++k) hexOk = hexOk && std::isxdigit(static_cast<unsigned char>(s[static_cast<size_t>(k)]));
            if (hexOk) {
                const auto c = hex(static_cast<unsigned>(std::stoul(s.substr(1, 6), nullptr, 16)));
                rows.push_back({c.r * 255, c.g * 255, c.b * 255});
                continue;
            }
        }
        if (s[0] == '#') continue;
        for (char& c : s)
            if (c == ',' || c == ';' || c == '\t') c = ' ';
        std::istringstream ls(s);
        std::vector<double> vals;
        std::string tok;
        while (ls >> tok) {
            try {
                size_t used = 0;
                vals.push_back(std::stod(tok, &used));
                if (used != tok.size()) throw std::invalid_argument("x");
            } catch (...) {
                if (rows.empty() && vals.empty()) goto nextLine;  // tolerate one header line
                throw std::invalid_argument("Line " + std::to_string(lineNo) + ": \"" + tok + "\" is not a number");
            }
        }
        if (vals.size() != 3 && vals.size() != 4)
            throw std::invalid_argument("Line " + std::to_string(lineNo) + ": expected r,g,b or t,r,g,b");
        rows.push_back(vals);
    nextLine:;
    }
    if (rows.size() < 2) throw std::invalid_argument("A palette needs at least two color stops");
    const size_t w = rows.front().size();
    for (const auto& r : rows)
        if (r.size() != w) throw std::invalid_argument("Mixing r,g,b and t,r,g,b lines is not allowed");
    const size_t off = w == 4 ? 1 : 0;
    double mx = 0;
    for (const auto& r : rows)
        for (size_t k = off; k < w; ++k) {
            if (r[k] < 0) throw std::invalid_argument("Color components must not be negative");
            mx = std::max(mx, r[k]);
        }
    const double scale = mx > 1.0 ? 1.0 / 255.0 : 1.0;
    std::vector<std::pair<double, Rgb>> stops;
    for (size_t i = 0; i < rows.size(); ++i) {
        const double t = w == 4 ? rows[i][0] : double(i) / double(rows.size() - 1);
        stops.push_back({t, {clamp01(rows[i][off] * scale), clamp01(rows[i][off + 1] * scale), clamp01(rows[i][off + 2] * scale)}});
    }
    if (w == 4) {
        for (size_t i = 1; i < stops.size(); ++i)
            if (stops[i].first < stops[i - 1].first) throw std::invalid_argument("Stop positions t must increase");
        const double t0 = stops.front().first, t1 = stops.back().first;
        if (!(t1 > t0)) throw std::invalid_argument("Stop positions t must span a non-empty interval");
        for (auto& s : stops) s.first = (s.first - t0) / (t1 - t0);
    }
    Colormap m = fromStops(name, stops, false);
    m.custom = true;
    // Treat as diverging when the middle is markedly lighter than both ends (white/grey midpoint).
    auto lum = [](Rgb c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
    const Rgb mid = m.sample(0.5);
    m.diverging = lum(mid) > lum(m.table.front()) + 0.25 && lum(mid) > lum(m.table.back()) + 0.25;
    return m;
}

}  // namespace abss::viz
