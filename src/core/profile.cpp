#include "abss/core/profile.hpp"

#include <cstdio>
#include <sstream>

namespace abss::prof {

Registry& Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(const std::string& category, double seconds, long long count) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(m_);
    auto& e = e_[category];
    e.count += count;
    e.seconds += seconds;
}

void Registry::reset() {
    std::lock_guard<std::mutex> lk(m_);
    e_.clear();
}

std::map<std::string, Entry> Registry::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return e_;
}

std::string Registry::toJson() const {
    const auto s = snapshot();
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (const auto& [k, v] : s) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "{\"count\": %lld, \"seconds\": %.6f}", v.count, v.seconds);
        os << (first ? "" : ",") << "\n  \"" << k << "\": " << buf;
        first = false;
    }
    os << "\n}";
    return os.str();
}

namespace {
thread_local std::string tlActivity;
}

const std::string& activity() { return tlActivity; }
void setActivity(const std::string& a) { tlActivity = a; }

Activity::Activity(std::string a) : prev_(tlActivity) { tlActivity = std::move(a); }
Activity::~Activity() { tlActivity = prev_; }

}  // namespace abss::prof
