#include "abss/core/profile.hpp"
#include "abss/oracle/oracle.hpp"

#include <future>

namespace abss {

void Oracle::runWave(int frame, std::vector<WaveRequest>& wave, bool concurrent) {
    auto one = [&](size_t t) {
        auto& w = wave[t];
        if (w.commit) {
            if (!w.nextProbes.empty() && w.branch->canFuseNextProbe(frame)) {
                prof::Activity act(w.activity + "_fused");
                w.fields = w.branch->commitAndProbe(frame, w.commitU, w.nextProbes, w.nextResiduals);
            } else {
                prof::Activity act(w.activity);
                w.fields = w.branch->commit(frame, w.commitU, nullptr);
                w.nextResiduals.clear();
            }
        } else if (!w.probes.empty()) {
            prof::Activity act(w.activity);
            w.residuals = w.branch->residuals(frame, w.probes);
        }
    };
    if (!concurrent || wave.size() < 2) {
        for (size_t t = 0; t < wave.size(); ++t) one(t);
        return;
    }
    std::vector<std::future<void>> fut;
    for (size_t t = 0; t < wave.size(); ++t) fut.push_back(std::async(std::launch::async, one, t));
    std::exception_ptr err;
    for (auto& f : fut) {
        try {
            f.get();
        } catch (...) {
            if (!err) err = std::current_exception();
        }
    }
    if (err) std::rethrow_exception(err);
}

}  // namespace abss
