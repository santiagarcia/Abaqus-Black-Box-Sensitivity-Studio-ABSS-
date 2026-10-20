// Material bridge: run a compiled material library that Abaqus cannot load itself (a Linux .so under WSL, or a
// Windows DLL with a non-Abaqus interface) behind a generated UMAT/SDVINI. See abaqus_matbridge.cpp.
#pragma once

#include "abss/oracle/abaqus.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace abss::detail {

using LogFn = std::function<void(const std::string&)>;

/// True for inputs handled by the bridge: an ELF shared object, or a .dll that is not an Abaqus standardU.dll.
bool isForeignMaterialLibrary(const std::filesystem::path& p);

/// Exported function names of an ELF64 shared object (.dynsym, defined FUNC symbols).
std::vector<std::string> elfExportNames(const std::filesystem::path& so);

/// Build the bridge library folder in `dir`:
///   standardU.dll (+ abss_abqstub.dll) with umat/sdvini forwarding to a helper process,
///   abss_matbridge.py (helper), abss_bridge.cmd (helper command line), a copy of the foreign library.
/// `buildC(src)` must compile and link a C source into dir/standardU.dll (the direct build).
/// `peExports(dll)` lists a Windows DLL's exports. `abaqusHome` locates Abaqus's bundled Python.
UserSubroutineLibrary prepareMaterialBridge(const std::filesystem::path& abaqusHome, const std::filesystem::path& lib,
                                            const std::filesystem::path& dir,
                                            const std::function<void(const std::filesystem::path&)>& buildC,
                                            const std::function<std::vector<std::string>(const std::filesystem::path&)>& peExports,
                                            const LogFn& log);

/// Linux Abaqus: generate a C UMAT/SDVINI wrapper that dlopen()s the foreign library in-process and calls it
/// (regular or UMAT ABI, size checks at every call), and build it into libstandardU.so with `abaqus make`.
UserSubroutineLibrary prepareMaterialBridgePosix(const std::string& abaqusCommand, const std::filesystem::path& lib,
                                                 const std::filesystem::path& dir, const LogFn& log);

}  // namespace abss::detail
