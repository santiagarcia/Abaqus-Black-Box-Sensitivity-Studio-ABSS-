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

/// Fortran/C toolchain that Abaqus needs to compile or link user subroutines (Windows: MSVC LINK + Intel Fortran).
struct CompilerToolchain {
    bool msvc = false;                ///< Visual Studio C++ tools (vcvars64.bat) found
    bool intelFortran = false;        ///< Intel oneAPI Fortran found
    std::string fortranCompiler;      ///< "ifort" or "ifx" (Abaqus 2024 expects ifort; ifx is configured automatically)
    std::string vcvars;               ///< path of vcvars64.bat
    std::string oneapiSetvars;        ///< path of oneAPI setvars.bat
    bool active = false;              ///< environment applied to this process (child Abaqus jobs inherit it)
    std::string message;              ///< plain-language status
};
/// Locate Visual Studio (vswhere) and Intel oneAPI. Fast; does not modify anything.
CompilerToolchain detectCompilerToolchain();
/// Load vcvars64 + oneAPI setvars into this process' environment so Abaqus jobs can compile/link. Idempotent.
bool activateCompilerToolchain(CompilerToolchain& tc);

/// A user-subroutine library that Abaqus loads through usub_lib_dir (no user= and no compiler needed per job).
struct UserSubroutineLibrary {
    std::string libraryDir;   ///< directory containing standardU.dll
    std::string kind;         ///< "prebuilt library", "object file", "Fortran source", "C/C++ source"
    bool builtHere = false;   ///< compiled/linked by ABSS with `abaqus make`
    std::string message;
    // Foreign material library (material bridge): requirements the model must meet, 0 = no requirement.
    int requiredNtens = 0, requiredNstatv = 0, requiredNprops = 0;
    bool requiresSdvini = false;  ///< the library initialises its state itself (deck needs *INITIAL CONDITIONS, TYPE=SOLUTION, USER)
};
/// Accepts: a folder or standardU.dll (prebuilt, no compiler needed), an object file (.obj/.o: needs LINK) or source
/// (.f/.for/.f90/.c/.cpp: needs the compilers). Objects/sources are built ONCE into `buildDir`.
/// Also accepts a FOREIGN material library that Abaqus cannot load itself - a Linux shared object (.so, run under
/// WSL) or a Windows .dll that is not an Abaqus standardU.dll - exporting either a standard UMAT (`umat_`/`umat`,
/// optional `sdvini`) or a deformation-gradient-driven batched update (`<name>(n, nprops, ntens, nstatv, props,
/// stress, statev, ddsdde, f0, f1, dtime, time, temp, dtemp, coords, celent, noel, npt, kstep, kinc, pnewdt)` plus
/// `<name>sdvini(n, nstatv, statev, coords, noel, npt)`). ABSS then builds a bridge standardU.dll whose UMAT/SDVINI
/// forward every call to a helper process that runs the library (material bridge).
/// Throws std::runtime_error with a plain-language explanation when this is impossible on this computer.
UserSubroutineLibrary prepareUserSubroutine(const std::string& abaqusCommand, const std::string& input,
                                            const std::string& buildDir, const std::function<void(const std::string&)>& log);

struct AbaqusRunConfig {
    std::string inpPath;                     ///< user's root INP (never modified)
    std::string abaqusCommand;
    std::string scratchDir;                  ///< all generated decks/jobs live here
    int cpus = 1;
    int maxConcurrentJobs = 1;               ///< licence-aware limit
    std::vector<ParameterInfo> parameters;   ///< discovered/selected parameters (index = delta index)
    std::string userSubroutine;              ///< UMAT/UEL: folder or standardU.dll, .obj/.o, or .f/.for/.f90/.c/.cpp
    bool keepJobFiles = false;
    std::function<void(const std::string&)> log;
    std::atomic<bool>* cancel = nullptr;
};

/// Capability matrix of the real backend, with what has been verified on this machine (see docs/VALIDATION_RESULTS.md).
std::vector<std::pair<std::string, std::string>> abaqusCapabilities();

/// Real oracle: nominal job, tangent extraction, frozen-displacement residual probes, history-branch replay.
std::unique_ptr<Oracle> makeAbaqusOracle(const AbaqusRunConfig& cfg);

}  // namespace abss
