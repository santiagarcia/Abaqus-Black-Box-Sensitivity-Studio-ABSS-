// ABSS core: lightweight run profiler (wall time + operation counts per category), thread-safe.
//
// Categories are dotted names ("engine.factorize", "job.wait_slot", ...). Every Abaqus job is attributed to the
// activity that requested it (probe.replay, probe.branch, commit, tangent.probe, gmres.matvec, validation.rerun,
// fafd.analysis, ...) through a per-thread activity label that callers set with prof::Activity.
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace abss::prof {

struct Entry {
    long long count = 0;
    double seconds = 0.0;  ///< summed wall time of the operations (concurrent operations add up)
};

/// Global registry. Values are summed over threads; elapsed (wall-clock) phases are recorded as separate categories.
class Registry {
public:
    static Registry& instance();
    void add(const std::string& category, double seconds, long long count = 1);
    void reset();
    std::map<std::string, Entry> snapshot() const;
    /// JSON object {"category": {"count": n, "seconds": s}, ...}
    std::string toJson() const;
    void enable(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

private:
    mutable std::mutex m_;
    std::map<std::string, Entry> e_;
    std::atomic<bool> enabled_{true};
};

/// Current activity of this thread (attribution of Abaqus jobs). Empty = "unattributed".
const std::string& activity();
void setActivity(const std::string& a);

/// RAII: set this thread's activity for a scope.
class Activity {
public:
    explicit Activity(std::string a);
    ~Activity();
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;

private:
    std::string prev_;
};

/// RAII: add the scope's wall time to a category.
class Scope {
public:
    explicit Scope(std::string category) : cat_(std::move(category)), t0_(std::chrono::steady_clock::now()) {}
    ~Scope() { Registry::instance().add(cat_, seconds()); }
    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::string cat_;
    std::chrono::steady_clock::time_point t0_;
};

inline void add(const std::string& category, double seconds, long long count = 1) {
    Registry::instance().add(category, seconds, count);
}

}  // namespace abss::prof
