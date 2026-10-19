// Safe subprocess execution for Abaqus jobs: argument vector (no shell string building from user text), working
// directory, timeout, cancellation, merged output capture. Only the process tree we start is ever terminated.
#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace abss::detail {

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    std::string output;
};

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
                         int timeoutSeconds, std::atomic<bool>* cancel);

std::optional<std::string> which(const std::string& exe);

}  // namespace abss::detail
