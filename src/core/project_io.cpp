#include "abss/core/project_io.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace abss {

using nlohmann::json;

namespace {

constexpr char kMagic[8] = {'A', 'B', 'S', 'S', 'R', 'E', 'S', '1'};

// ---- SHA-256 (FIPS 180-4), compact implementation for provenance hashes ----
struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint64_t bits = 0;
    std::array<unsigned char, 64> buf{};
    size_t len = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const unsigned char* p, size_t n) {
        bits += std::uint64_t(n) * 8;
        while (n--) {
            buf[len++] = *p++;
            if (len == 64) { block(buf.data()); len = 0; }
        }
    }
    std::string hex() {
        std::uint64_t b = bits;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (len != 56) update(&z, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<unsigned char>(b >> (56 - 8 * i));
        update(lenb, 8);
        std::ostringstream os;
        for (auto v : h) os << std::hex << std::setw(8) << std::setfill('0') << v;
        return os.str();
    }
};

json fieldToJson(const FieldDescriptor& f) {
    return {{"name", f.name}, {"location", static_cast<int>(f.location)}, {"kind", static_cast<int>(f.kind)},
            {"components", f.components}, {"description", f.description}, {"units", f.units}};
}
FieldDescriptor fieldFromJson(const json& j) {
    FieldDescriptor f;
    f.name = j.at("name");
    f.location = static_cast<FieldLocation>(j.at("location").get<int>());
    f.kind = static_cast<FieldKind>(j.at("kind").get<int>());
    f.components = j.at("components").get<std::vector<std::string>>();
    f.description = j.value("description", "");
    f.units = j.value("units", "");
    return f;
}

json paramToJson(const ParameterInfo& p) {
    json t = json::array();
    for (const auto& tg : p.targets)
        t.push_back({{"file", tg.ref.file}, {"line", tg.ref.line}, {"field", tg.ref.field}, {"keyword", tg.ref.keyword},
                     {"nominal", tg.nominalToken}, {"weight", tg.weight}});
    return {{"id", p.id}, {"name", p.name}, {"description", p.description}, {"category", static_cast<int>(p.category)},
            {"confidence", static_cast<int>(p.confidence)}, {"nominal", p.nominal}, {"units", p.units},
            {"positive", p.mustBePositive}, {"lower", p.lowerBound}, {"upper", p.upperBound},
            {"relativeStep", p.relativeStep}, {"association", p.association}, {"steps", p.steps},
            {"mode", static_cast<int>(p.mode)}, {"targets", t}, {"supported", p.supported}, {"note", p.note}};
}
ParameterInfo paramFromJson(const json& j) {
    ParameterInfo p;
    p.id = j.at("id");
    p.name = j.at("name");
    p.description = j.value("description", "");
    p.category = static_cast<ParamCategory>(j.at("category").get<int>());
    p.confidence = static_cast<Confidence>(j.at("confidence").get<int>());
    p.nominal = j.at("nominal");
    p.units = j.value("units", "");
    p.mustBePositive = j.value("positive", false);
    p.lowerBound = j.value("lower", -1e300);
    p.upperBound = j.value("upper", 1e300);
    p.relativeStep = j.value("relativeStep", 1e-3);
    p.association = j.value("association", "");
    p.steps = j.value("steps", std::vector<std::string>{});
    p.mode = static_cast<EditMode>(j.value("mode", 0));
    for (const auto& t : j.value("targets", json::array())) {
        ParameterTarget tg;
        tg.ref.file = t.at("file");
        tg.ref.line = t.at("line");
        tg.ref.field = t.at("field");
        tg.ref.keyword = t.value("keyword", "");
        tg.nominalToken = t.at("nominal");
        tg.weight = t.value("weight", 1.0);
        p.targets.push_back(tg);
    }
    p.supported = j.value("supported", true);
    p.note = j.value("note", "");
    return p;
}

json qualityToJson(const QualityRecord& q) {
    return {{"status", static_cast<int>(q.status)}, {"fdRelativeError", q.fdRelativeError},
            {"predictorResidualRatio", q.predictorResidualRatio}, {"tangentCheckError", q.tangentCheckError},
            {"stepSize", q.stepSize}, {"scheme", q.scheme}, {"notes", q.notes}, {"suspectFrames", q.suspectFrames}};
}
QualityRecord qualityFromJson(const json& j) {
    QualityRecord q;
    q.status = static_cast<QualityStatus>(j.at("status").get<int>());
    q.fdRelativeError = j.value("fdRelativeError", -1.0);
    q.predictorResidualRatio = j.value("predictorResidualRatio", -1.0);
    q.tangentCheckError = j.value("tangentCheckError", -1.0);
    q.stepSize = j.value("stepSize", 0.0);
    q.scheme = j.value("scheme", "");
    q.notes = j.value("notes", std::vector<std::string>{});
    q.suspectFrames = j.value("suspectFrames", std::vector<int>{});
    return q;
}

// JSON cannot hold inf/nan; doubles in the manifest are finite by construction (bounds use +-1e300).

}  // namespace

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), buf.size());
        auto n = static_cast<size_t>(in.gcount());
        if (n) s.update(reinterpret_cast<const unsigned char*>(buf.data()), n);
    }
    return s.hex();
}

void saveProject(const std::string& path, const ResultStore& store, const ProjectMeta& meta) {
    json m;
    m["format"] = "ABSSRES1";
    m["meta"] = {{"appVersion", meta.appVersion}, {"createdUtc", meta.createdUtc}, {"inpPath", meta.inpPath},
                 {"inpSha256", meta.inpSha256}, {"dependencies", meta.dependencies},
                 {"solverBackend", meta.solverBackend}, {"computeBackend", meta.computeBackend},
                 {"settings", meta.settingsJson}};
    m["title"] = store.title;
    m["sourceInp"] = store.sourceInp;
    m["backend"] = store.backend;
    m["synthetic"] = store.synthetic;
    m["log"] = store.log;

    std::vector<double> payload;
    auto appendBlock = [&](const std::vector<double>& v) {
        json b = {{"offset", payload.size() * sizeof(double)}, {"count", v.size()}};
        payload.insert(payload.end(), v.begin(), v.end());
        return b;
    };

    const Mesh& mesh = store.mesh;
    json jm;
    jm["name"] = mesh.name;
    jm["spatialDim"] = mesh.spatialDim;
    jm["instances"] = mesh.instanceNames;
    std::vector<double> nk;
    nk.reserve(mesh.nodeKeys.size() * 2);
    for (const auto& k : mesh.nodeKeys) {
        nk.push_back(k.instance);
        nk.push_back(static_cast<double>(k.label));
    }
    jm["nodeKeys"] = appendBlock(nk);
    jm["xyz"] = appendBlock(mesh.xyz);
    json je = json::array();
    std::vector<double> conn;
    for (const auto& e : mesh.elements) {
        je.push_back({e.label, e.instance, e.type, static_cast<int>(e.info.shape), static_cast<int>(e.info.family),
                      e.info.topoDim, e.info.cornerNodes, e.conn.size(), e.integrationPoints});
        for (auto c : e.conn) conn.push_back(c);
    }
    jm["elements"] = je;
    jm["conn"] = appendBlock(conn);
    m["mesh"] = jm;

    std::vector<double> po(store.points.pointOffset.begin(), store.points.pointOffset.end());
    std::vector<double> ip(store.points.ipNumber.begin(), store.points.ipNumber.end());
    std::vector<double> sp(store.points.sectionPoint.begin(), store.points.sectionPoint.end());
    m["points"] = {{"offset", appendBlock(po)}, {"ip", appendBlock(ip)}, {"sp", appendBlock(sp)}};

    json jf = json::array();
    for (const auto& f : store.frames)
        jf.push_back({{"step", f.step}, {"stepName", f.stepName}, {"increment", f.increment},
                      {"stepTime", f.stepTime}, {"totalTime", f.totalTime}});
    m["frames"] = jf;
    json fields = json::array();
    for (const auto& f : store.fields) fields.push_back(fieldToJson(f));
    m["fields"] = fields;
    json params = json::array();
    for (const auto& p : store.parameters) params.push_back(paramToJson(p));
    m["parameters"] = params;
    json quality = json::array();
    for (const auto& [d, q] : store.quality) quality.push_back({{"params", d.params}, {"record", qualityToJson(q)}});
    m["quality"] = quality;

    json series = json::array();
    store.forEachSeries([&](int field, const DerivativeSpec& d, int frame, const std::vector<double>& v) {
        series.push_back({{"field", field}, {"params", d.params}, {"frame", frame}, {"data", appendBlock(v)}});
    });
    m["series"] = series;

    const std::string text = m.dump();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write project file: " + path);
    out.write(kMagic, 8);
    std::uint64_t n = text.size();
    out.write(reinterpret_cast<const char*>(&n), sizeof n);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size() * sizeof(double)));
    if (!out) throw std::runtime_error("Write failed: " + path);
}

ResultStore loadProject(const std::string& path, ProjectMeta* meta) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open project file: " + path);
    char magic[8];
    in.read(magic, 8);
    if (!in || std::memcmp(magic, kMagic, 8) != 0) throw std::runtime_error("Not an ABSS results file: " + path);
    std::uint64_t n = 0;
    in.read(reinterpret_cast<char*>(&n), sizeof n);
    if (!in || n > (1ULL << 34)) throw std::runtime_error("Corrupt ABSS results header: " + path);
    std::string text(n, '\0');
    in.read(text.data(), static_cast<std::streamsize>(n));
    std::vector<char> payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json m = json::parse(text);

    auto block = [&](const json& b) {
        const auto off = b.at("offset").get<size_t>();
        const auto cnt = b.at("count").get<size_t>();
        if (off + cnt * sizeof(double) > payload.size()) throw std::runtime_error("Corrupt ABSS payload: " + path);
        std::vector<double> v(cnt);
        if (cnt) std::memcpy(v.data(), payload.data() + off, cnt * sizeof(double));
        return v;
    };

    ResultStore s;
    s.title = m.value("title", "");
    s.sourceInp = m.value("sourceInp", "");
    s.backend = m.value("backend", "");
    s.synthetic = m.value("synthetic", false);
    s.log = m.value("log", std::vector<std::string>{});
    if (meta) {
        const auto& j = m.at("meta");
        meta->appVersion = j.value("appVersion", "");
        meta->createdUtc = j.value("createdUtc", "");
        meta->inpPath = j.value("inpPath", "");
        meta->inpSha256 = j.value("inpSha256", "");
        meta->dependencies = j.value("dependencies", std::vector<std::string>{});
        meta->solverBackend = j.value("solverBackend", "");
        meta->computeBackend = j.value("computeBackend", "");
        meta->settingsJson = j.value("settings", "");
    }
    const auto& jm = m.at("mesh");
    s.mesh.name = jm.value("name", "");
    s.mesh.spatialDim = jm.value("spatialDim", 3);
    s.mesh.instanceNames = jm.value("instances", std::vector<std::string>{});
    const auto nk = block(jm.at("nodeKeys"));
    const auto xyz = block(jm.at("xyz"));
    for (size_t i = 0; i < nk.size() / 2; ++i)
        s.mesh.addNode({static_cast<std::int32_t>(nk[2 * i]), static_cast<std::int64_t>(nk[2 * i + 1])},
                       {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]});
    const auto conn = block(jm.at("conn"));
    size_t c = 0;
    for (const auto& e : jm.at("elements")) {
        Element el;
        el.label = e[0];
        el.instance = e[1];
        el.type = e[2];
        el.info.shape = static_cast<ElementShape>(e[3].get<int>());
        el.info.family = static_cast<ElementFamily>(e[4].get<int>());
        el.info.topoDim = e[5];
        el.info.cornerNodes = e[6];
        const auto nn = e[7].get<size_t>();
        el.integrationPoints = e[8];
        for (size_t k = 0; k < nn; ++k) el.conn.push_back(static_cast<std::int32_t>(conn.at(c++)));
        s.mesh.elements.push_back(std::move(el));
    }
    for (double v : block(m.at("points").at("offset"))) s.points.pointOffset.push_back(static_cast<std::int32_t>(v));
    for (double v : block(m.at("points").at("ip"))) s.points.ipNumber.push_back(static_cast<std::int16_t>(v));
    for (double v : block(m.at("points").at("sp"))) s.points.sectionPoint.push_back(static_cast<std::int16_t>(v));
    for (const auto& f : m.at("frames"))
        s.frames.push_back({f.at("step"), f.value("stepName", ""), f.at("increment"), f.at("stepTime"), f.at("totalTime")});
    for (const auto& f : m.at("fields")) s.fields.push_back(fieldFromJson(f));
    for (const auto& p : m.at("parameters")) s.parameters.push_back(paramFromJson(p));
    for (const auto& q : m.at("quality"))
        s.quality[DerivativeSpec{q.at("params").get<std::vector<int>>()}] = qualityFromJson(q.at("record"));
    for (const auto& e : m.at("series"))
        s.set(e.at("field"), DerivativeSpec{e.at("params").get<std::vector<int>>()}, e.at("frame"), block(e.at("data")));
    return s;
}

std::string historyCsv(const ResultStore& store, int field, std::int64_t location, int component,
                       const std::vector<DerivativeSpec>& series) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17);
    const auto& f = store.fields.at(static_cast<size_t>(field));
    const std::string q = f.components.at(static_cast<size_t>(component));
    os << "frame,step,increment,step_time,total_time";
    for (const auto& d : series) os << ",\"" << store.derivativeLabel(q, d) << "\"";
    os << "\n";
    std::vector<std::vector<double>> h;
    for (const auto& d : series) h.push_back(store.history(field, d, location, component));
    for (size_t i = 0; i < store.frames.size(); ++i) {
        const auto& fr = store.frames[i];
        os << i << ',' << fr.step << ',' << fr.increment << ',' << fr.stepTime << ',' << fr.totalTime;
        for (const auto& col : h) {
            os << ',';
            if (!std::isnan(col[i])) os << col[i];
        }
        os << "\n";
    }
    return os.str();
}

}  // namespace abss
