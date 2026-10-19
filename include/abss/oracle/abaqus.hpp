// ABSS Abaqus backend: detection, licence probe, and the real residual/tangent oracle driving Abaqus jobs.
// All processing is local; no file content is ever sent to a network service.
#pragma once

#include "abss/oracle/oracle.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace abss {

struct AbaqusInstallation {
    bool found = false;
    std::string command;       ///< e.g. C:\SIMULIA\Commands\abaqus.bat
    std::string version;       ///< e.g. "Abaqus 2024"
    bool licenseChecked = false;
    bool licenseOk = false;
    bool userSubroutinesCompilable = false;  ///< Fortran/C toolchain usable by `abaqus make` / user=
    std::string message;       ///< plain-language status
};

/// Locate Abaqus (ABSS_ABAQUS_CMD env, PATH, standard SIMULIA command directories). Fast; runs `abaqus information=release`.
AbaqusInstallation detectAbaqus(const std::string& overrideCommand = "");
/// Run a tiny 1-element job in a temporary folder to confirm a licence token can be obtained (uses one briefly).
bool checkAbaqusLicense(AbaqusInstallation& inst, const std::string& scratchDir);

struct AbaqusRunConfig {
    std::string inpPath;                     ///< user's root INP (never modified)
    std::string abaqusCommand;
    std::string scratchDir;                  ///< all generated decks/jobs live here
    int cpus = 1;
    int maxConcurrentJobs = 1;               ///< licence-aware limit
    std::vector<ParameterInfo> parameters;   ///< discovered/selected parameters (index = delta index)
    std::string userSubroutine;              ///< optional precompiled object/library or source for user=
    bool keepJobFiles = false;
    std::function<void(const std::string&)> log;
    std::atomic<bool>* cancel = nullptr;
};

/// Capability matrix of the real backend, with what has been verified on this machine (see docs/VALIDATION_RESULTS.md).
std::vector<std::pair<std::string, std::string>> abaqusCapabilities();

/// Real oracle: nominal job, tangent extraction, frozen-displacement residual probes, history-branch replay.
std::unique_ptr<Oracle> makeAbaqusOracle(const AbaqusRunConfig& cfg);

}  // namespace abss
