#include "abss/sensitivity/engine.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace abss {

namespace {

/// Max relative error of a stored series vs. a reference computed per (field, frame) by `ref`.
/// Error scale: the reference's magnitude, floored by the field's physical scale max|F_nominal| / |a|^k.
/// Outcome of comparing one derivative series against its rerun reference. "Validated" requires compared > 0,
/// missingFrames == 0 and nonFinite == 0 (Vera F2: NaN, missing series and empty comparisons never pass).
struct Comparison {
    double worst = 0;
    std::string field;
    long long compared = 0;
    long long nonFinite = 0;   ///< engine or reference NaN/Inf where the nominal value exists
    int missingFrames = 0;     ///< frames without an engine value
    // Where the worst error is (for diagnosis): increment, flat component index, engine and reference values, scale.
    size_t worstFrame = 0, worstIndex = 0;
    double worstEngine = 0, worstRef = 0, worstScale = 0;
    std::string perField;  ///< "U 1e-7, RF 2e-6, ..." relative error of every compared field
};

template <class Ref>
Comparison compare(const ResultStore& store, const DerivativeSpec& spec, const ValidationOptions& opt,
                   const std::vector<FrameFields>& nominalFields, double paramScaleK, int order, Ref&& ref) {
    Comparison out;
    double& worst = out.worst;
    std::string& worstField = out.field;
    const size_t nFrames = nominalFields.size();
    for (size_t f = 0; f < store.fields.size(); ++f) {
        if (!opt.fields.empty() && std::find(opt.fields.begin(), opt.fields.end(), store.fields[f].name) == opt.fields.end())
            continue;
        double nominalMax = 0;
        for (size_t fr = 1; fr < nFrames; ++fr)
            for (double v : nominalFields[fr].values[f])
                if (std::isfinite(v)) nominalMax = std::max(nominalMax, std::abs(v));
        // Absolute floor: an error below floorK * max|F| / a^k changes the order-k Taylor term by a negligible amount
        // (5e-7 of the field for a 1 % parameter change at order 1). It must also exceed the scheme's own truncation
        // error: commits use the order-K predictor, so a derivative carries ~h_rel * max|F| / a^k (h_rel = 1e-5 at
        // K = 1). The former 1e-4 (threshold 1e-7 at tolerance 1e-3) sat below that, so derivatives that are nearly
        // zero (e.g. dS/dE of a force-driven single-material body, measured 2e-5 of max|S|/E) could never pass.
        static const double floorK[] = {5e-2, 5e-2, 5e-2, 1e-1, 1e-1};
        double scale = floorK[std::min(order, 4)] * nominalMax / paramScaleK;
        double err = 0;
        size_t errFrame = 0, errIndex = 0;
        double errEngine = 0, errRef = 0;
        for (size_t fr = 1; fr < nFrames; ++fr) {
            const auto* s = store.get(static_cast<int>(f), spec, static_cast<int>(fr));
            if (!s) {
                if (f == 0) ++out.missingFrames;  // count once per frame (field 0 always exists)
                continue;
            }
            const auto& nom = nominalFields[fr].values[f];
            for (size_t c = 0; c < s->size(); ++c) {
                const bool exists = c < nom.size() && std::isfinite(nom[c]);  // e.g. no S output for some elements
                const double r = ref(f, fr, c);
                if (!std::isfinite((*s)[c]) || !std::isfinite(r)) {
                    if (exists) ++out.nonFinite;
                    continue;
                }
                ++out.compared;
                scale = std::max(scale, std::abs(r));
                if (std::abs((*s)[c] - r) > err) {
                    err = std::abs((*s)[c] - r);
                    errFrame = fr, errIndex = c, errEngine = (*s)[c], errRef = r;
                }
            }
        }
        const double rel = scale > 0 ? err / scale : err;
        {
            std::ostringstream fe;
            fe.precision(2);
            fe << (out.perField.empty() ? "" : ", ") << store.fields[f].name << " " << rel;
            out.perField += fe.str();
        }
        if (rel > worst) {
            worst = rel;
            worstField = store.fields[f].name;
            out.worstFrame = errFrame, out.worstIndex = errIndex, out.worstEngine = errEngine, out.worstRef = errRef;
            out.worstScale = scale;
        }
    }
    return out;
}

void grade(QualityRecord& q, const Comparison& cmp, double tol, double h, int order, std::ostringstream& summary,
           const std::string& label) {
    q.fdRelativeError = cmp.worst;
    std::ostringstream note;
    note.precision(3);
    if (cmp.compared == 0 || cmp.missingFrames > 0 || cmp.nonFinite > 0) {
        q.status = cmp.compared == 0 && cmp.missingFrames == 0 && cmp.nonFinite == 0 ? QualityStatus::Unverified
                                                                                      : QualityStatus::Failed;
        note << "Full-rerun check could not validate: " << cmp.compared << " values compared, " << cmp.missingFrames
             << " increment(s) missing, " << cmp.nonFinite << " non-finite value(s) where data exists.";
        q.notes.push_back(note.str());
        summary << label << ": NOT VALIDATED (" << cmp.compared << " compared, " << cmp.missingFrames << " missing, "
                << cmp.nonFinite << " non-finite)\n";
        return;
    }
    const bool pass = cmp.worst <= tol;
    q.status = pass ? (order <= 2 ? QualityStatus::Validated : QualityStatus::Experimental) : QualityStatus::Failed;
    note << "Full-rerun central FD check (h=" << h << ", all increments, " << cmp.compared << " values): max relative error "
         << cmp.worst << " (worst field " << cmp.field << ", tolerance " << tol << ") -> " << (pass ? "pass" : "FAIL");
    note << ". Per field: " << cmp.perField;
    if (!pass)
        note << ". Worst value: increment " << cmp.worstFrame << ", component index " << cmp.worstIndex << ": engine "
             << cmp.worstEngine << ", rerun FD " << cmp.worstRef << " (error scale " << cmp.worstScale << ")";
    q.notes.push_back(note.str());
    summary << label << ": max rel. error " << cmp.worst << " [" << cmp.field << "] " << (pass ? "PASS" : "FAIL") << "\n";
}

}  // namespace

std::string validateAgainstReruns(Oracle& oracle, const SensitivityRequest& req, ResultStore& store,
                                  const ValidationOptions& opt, RunControl& ctl) {
    std::ostringstream summary;
    summary.precision(3);
    const int K = req.maxOrder;
    const int M = req.halfWidth > 0 ? req.halfWidth : defaultHalfWidth(K);
    std::vector<int> offsets;
    for (int m = -M; m <= M; ++m) offsets.push_back(m);
    const size_t nAll = store.parameters.size();

    std::vector<FrameFields> nominalFields;
    if (oracle.solvePerturbed(std::vector<double>(nAll, 0.0), &nominalFields).empty()) {
        summary << "Validation skipped: nominal rerun failed.\n";
        return summary.str();
    }
    const size_t nFrames = nominalFields.size();
    std::vector<double> hOf(nAll, 0.0);

    for (size_t j = 0; j < req.parameters.size(); ++j) {
        if (ctl.cancel) break;
        const int pi = req.parameters[j];
        const auto& p = store.parameters.at(static_cast<size_t>(pi));
        const double hEngine = j < req.absoluteSteps.size() && req.absoluteSteps[j] > 0
                                   ? req.absoluteSteps[j]
                                   : defaultRelativeStep(K) * (p.absoluteStep() / p.relativeStep);
        const double hv = hEngine * opt.stepFactor;
        hOf[static_cast<size_t>(pi)] = hv;
        std::vector<std::vector<FrameFields>> runs(offsets.size());
        bool ok = true;
        for (size_t o = 0; o < offsets.size(); ++o) {
            if (offsets[o] == 0) {
                runs[o] = nominalFields;
                continue;
            }
            std::vector<double> delta(nAll, 0.0);
            delta[static_cast<size_t>(pi)] = offsets[o] * hv;
            if (oracle.solvePerturbed(delta, &runs[o]).empty() || runs[o].size() != nFrames) ok = false;
            if (ctl.progress) ctl.progress("Validation reruns (" + p.name + ")", double(o + 1) / offsets.size());
        }
        for (int k = 1; k <= K; ++k) {
            auto& q = store.quality[DerivativeSpec::pure(pi, k)];
            const std::string label = p.name + " order " + std::to_string(k);
            if (!ok) {
                q.status = QualityStatus::Failed;
                q.notes.push_back("Validation rerun did not converge (possible cutback/instability); derivative unverified.");
                summary << label << ": rerun failed\n";
                continue;
            }
            const auto wk = stencilWeights(offsets, k);
            const double hk = std::pow(hv, k);
            const auto cmp =
                compare(store, DerivativeSpec::pure(pi, k), opt, nominalFields,
                        std::pow(p.absoluteStep() / p.relativeStep, k), k, [&](size_t f, size_t fr, size_t c) {
                            double fd = 0;
                            for (size_t o = 0; o < offsets.size(); ++o) fd += wk[o] * runs[o][fr].values[f][c];
                            return fd / hk;
                        });
            const double tol = opt.tolerance.at(std::min<size_t>(static_cast<size_t>(k - 1), opt.tolerance.size() - 1));
            grade(q, cmp, tol, hv, k, summary, label);
        }
    }

    // Mixed second derivatives: 4-corner central FD of full reruns at (a +- h_a, b +- h_b).
    for (const auto& [a0, b0] : req.mixedPairs) {
        if (ctl.cancel) break;
        const int a = std::min(a0, b0), b = std::max(a0, b0);
        const double ha = hOf[static_cast<size_t>(a)], hb = hOf[static_cast<size_t>(b)];
        std::vector<FrameFields> c[4];
        bool ok = true;
        int idx = 0;
        for (int sa : {+1, -1})
            for (int sb : {+1, -1}) {
                std::vector<double> delta(nAll, 0.0);
                delta[static_cast<size_t>(a)] = sa * ha;
                delta[static_cast<size_t>(b)] = sb * hb;
                if (oracle.solvePerturbed(delta, &c[idx]).empty() || c[idx].size() != nFrames) ok = false;
                ++idx;
            }
        const DerivativeSpec spec{{a, b}};
        auto& q = store.quality[spec];
        const std::string label = "mixed " + store.parameters[static_cast<size_t>(a)].name + "," +
                                  store.parameters[static_cast<size_t>(b)].name;
        if (!ok) {
            q.status = QualityStatus::Failed;
            q.notes.push_back("Corner rerun did not converge; mixed derivative unverified.");
            summary << label << ": rerun failed\n";
            continue;
        }
        const auto& pa = store.parameters[static_cast<size_t>(a)];
        const auto& pb = store.parameters[static_cast<size_t>(b)];
        const double scaleK = (pa.absoluteStep() / pa.relativeStep) * (pb.absoluteStep() / pb.relativeStep);
        const auto cmp = compare(store, spec, opt, nominalFields, scaleK, 2, [&](size_t f, size_t fr, size_t cc) {
            return (c[0][fr].values[f][cc] - c[1][fr].values[f][cc] - c[2][fr].values[f][cc] + c[3][fr].values[f][cc]) /
                   (4 * ha * hb);
        });
        grade(q, cmp, opt.tolerance.at(std::min<size_t>(1, opt.tolerance.size() - 1)), ha, 2, summary, label);
    }
    return summary.str();
}

}  // namespace abss
