// User subroutines (UMAT/UEL) for the Abaqus oracle.
//
// Every probe/commit is a separate Abaqus job, so passing `user=` would compile and link the subroutine hundreds of
// times. Instead ABSS produces ONE library folder containing standardU.dll and every job loads it through
// `usub_lib_dir` in a job-directory abaqus_v6.env. Three inputs are accepted:
//   * prebuilt library (folder or standardU.dll): used as is - no source and no compiler needed;
//   * object file (.obj/.o): linked once;
//   * source (.f/.for/.f90 Fortran, .c/.cpp C/C++): compiled and linked once.
// Building uses `abaqus make` when the Abaqus installation contains its user-subroutine development files
// (aba_param.inc + import libraries). When it does not (verified on this Abaqus 2024 installation), ABSS performs a
// DIRECT BUILD: it compiles with the same flags Abaqus uses and links a standardU.dll whose user entry points come from
// the user's objects, while every other entry point the solver imports is a small lazy thunk into an unmodified private
// copy of Abaqus's own stub library (abss_abqstub.dll, loaded by absolute path). The Abaqus installation is never
// modified.
#include "abss/oracle/abaqus.hpp"

#include "abaqus_matbridge.hpp"
#include "abaqus_process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace abss {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
}

std::mutex gEnvMutex;

/// Newest versioned subfolder ("2025.1", "2024.2"...) of a oneAPI root, or empty.
fs::path newestVersionDir(const fs::path& root) {
    std::error_code ec;
    fs::path best;
    std::string bestName;
    static const std::regex ver(R"(^\d{4}\.\d+(\.\d+)?$)");
    for (const auto& e : fs::directory_iterator(root, ec)) {
        const auto n = e.path().filename().string();
        if (e.is_directory(ec) && std::regex_match(n, ver) && n > bestName) {
            bestName = n;
            best = e.path();
        }
    }
    return best;
}

/// Names exported by a PE DLL (reads the export directory; no external tools).
std::vector<std::string> peExportNames(const fs::path& dll) {
    std::ifstream f(dll, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { std::uint16_t v; std::memcpy(&v, b.data() + o, 2); return v; };
    auto u32 = [&](size_t o) { std::uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') throw std::runtime_error("Not a PE file: " + dll.string());
    const size_t pe = u32(0x3C);
    const size_t opt = pe + 24;
    const size_t nsec = u16(pe + 6);
    const size_t optSize = u16(pe + 20);
    const bool pe32plus = u16(opt) == 0x20B;
    const size_t dirs = opt + (pe32plus ? 112 : 96);
    const std::uint32_t expRva = u32(dirs);
    const size_t sec = opt + optSize;
    auto rva2off = [&](std::uint32_t rva) -> size_t {
        for (size_t i = 0; i < nsec; ++i) {
            const size_t s = sec + i * 40;
            const std::uint32_t va = u32(s + 12), vsz = std::max(u32(s + 8), u32(s + 16)), raw = u32(s + 20);
            if (rva >= va && rva < va + vsz) return raw + (rva - va);
        }
        throw std::runtime_error("Bad RVA in " + dll.string());
    };
    std::vector<std::string> names;
    if (!expRva) return names;
    const size_t ed = rva2off(expRva);
    const std::uint32_t n = u32(ed + 24), namesRva = u32(ed + 32);
    const size_t np = rva2off(namesRva);
    for (std::uint32_t i = 0; i < n; ++i) names.emplace_back(b.data() + rva2off(u32(np + 4 * i)));
    return names;
}

/// Abaqus installation directory (e.g. C:\SIMULIA\EstProducts\2024) from `abaqus information=release`.
fs::path abaqusHome(const std::string& abaqusCommand) {
    const auto tmp = fs::temp_directory_path() / "abss_probe";
    fs::create_directories(tmp);
    const auto r = detail::runProcess(abaqusCommand, {"information=release"}, tmp.string(), 120, nullptr);
    std::smatch m;
    if (std::regex_search(r.output, m, std::regex(R"(located in the directory\s+(\S+))"))) return fs::path(m[1].str());
    return {};
}

void writeFile(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write " + p.string());
    f << s;
}

detail::ProcessResult tool(const std::string& exe, const std::vector<std::string>& args, const fs::path& cwd,
                           const std::string& what) {
    const auto path = detail::which(exe);
    if (!path) throw std::runtime_error(what + ": " + exe + " was not found on PATH after loading the compiler environment.");
    auto r = detail::runProcess(*path, args, cwd.string(), 1800, nullptr);
    if (r.exitCode != 0)
        throw std::runtime_error(what + " failed (" + exe + ", exit code " + std::to_string(r.exitCode) + "):\n" +
                                 r.output.substr(r.output.size() > 2500 ? r.output.size() - 2500 : 0));
    return r;
}

/// Direct build of standardU.dll (see the file comment). Returns the library folder.
void directBuild(const fs::path& home, const fs::path& input, bool object, bool fortran, const fs::path& dir,
                 const CompilerToolchain& tc, const std::function<void(const std::string&)>& log) {
    const fs::path stub = home / "win_b64" / "code" / "bin" / "standardU.dll";
    if (!fs::exists(stub))
        throw std::runtime_error("Abaqus' own standardU.dll was not found at " + stub.string() + "; cannot build a compatible library.");
    // Abaqus/Standard double precision include file (the Abaqus-documented content).
    writeFile(dir / "aba_param.inc", "      implicit real*8(a-h,o-z)\r\n      parameter (nprecd=2)\r\n");
    // 1. objects
    fs::path obj;
    if (object) {
        obj = dir / input.filename();
        fs::copy_file(input, obj, fs::copy_options::overwrite_existing);
    } else {
        const fs::path src = dir / input.filename();
        fs::copy_file(input, src, fs::copy_options::overwrite_existing);
        if (fortran) {
            // Abaqus 2024 site flags (win86_64.env compile_fortran), minus options the compiler does not accept.
            std::vector<std::string> a = {"/nologo", "/c", "/fpp", "/extend-source", "/DABQ_WIN86_64", "/DABQ_FORTRAN",
                                          "/iface:cref", "/recursive", "/Qauto", "/align:array64byte", "/Qprec-div",
                                          "/Qfma-", "/fp:precise", "/reentrancy:threaded", "/libs:dll", "/threads",
                                          "/include:" + dir.string(), src.filename().string()};
            if (tc.fortranCompiler == "ifort") a.insert(a.end() - 1, {"/Qprec-sqrt", "/Qpc64"});
            tool(tc.fortranCompiler + ".exe", a, dir, "Fortran compilation");
        } else {
            tool("cl.exe", {"/nologo", "/c", "/MD", "/O2", "/EHsc", "/DABQ_WIN86_64", "/I" + dir.string(), src.filename().string()},
                 dir, "C/C++ compilation");
        }
        obj = dir / (src.stem().string() + ".obj");
        if (!fs::exists(obj)) throw std::runtime_error("The compiler did not produce " + obj.string());
    }
    // 2. which entry points do the user's objects define?
    const auto sym = tool("dumpbin.exe", {"/nologo", "/symbols", obj.string()}, dir, "Reading object symbols");
    std::set<std::string> defined;
    {
        std::istringstream in(sym.output);
        std::string l;
        while (std::getline(in, l))
            if (l.find("External") != std::string::npos && l.find("SECT") != std::string::npos &&
                l.find("UNDEF") == std::string::npos) {
                const auto bar = l.find('|');
                if (bar == std::string::npos) continue;
                std::istringstream n(l.substr(bar + 1));
                std::string name;
                n >> name;
                if (!name.empty()) defined.insert(name);
            }
    }
    // 3. the solver's entry points (all functions), from Abaqus' stub
    std::vector<std::string> exports;
    for (const auto& n : peExportNames(stub))
        if (n != "DllMain" && n != "_forceCRTManifestCUR" && n.rfind("DASSAULT_SYSTEMES_", 0) != 0) exports.push_back(n);
    std::vector<std::string> mine, thunked;
    for (const auto& n : exports) (defined.count(n) ? mine : thunked).push_back(n);
    if (mine.empty())
        throw std::runtime_error("The subroutine file defines none of the Abaqus user-subroutine entry points (e.g. umat, uel). "
                                 "Compiled names found: " + std::to_string(defined.size()) +
                                 ". Fortran routines must be compiled with /iface:cref (lower-case names).");
    if (log) {
        std::string s;
        for (const auto& n : mine) s += (s.empty() ? "" : ", ") + n;
        log("Direct build (Abaqus user-subroutine development files are not installed): user entry points " + s +
            "; " + std::to_string(thunked.size()) + " other entry points delegate to Abaqus' own stub.");
    }
    fs::copy_file(stub, dir / "abss_abqstub.dll", fs::copy_options::overwrite_existing);
    // 4. lazy thunks (MASM) + resolver (C, no CRT)
    std::ostringstream asmf, cf, def;
    asmf << "; Generated by ABSS: lazy thunks into abss_abqstub.dll (Abaqus' own stub, loaded by absolute path)\n"
            "EXTERN abss_resolve:PROC\nEXTERN abss_fp:QWORD\n.code\n";
    for (size_t i = 0; i < thunked.size(); ++i)
        asmf << "abss_t" << i << " PROC\n  mov rax, qword ptr [abss_fp + " << 8 * i << "]\n  test rax, rax\n  jz abss_s" << i
             << "\n  jmp rax\nabss_s" << i << ":\n  mov r10d, " << i << "\n  jmp abss_slow\nabss_t" << i << " ENDP\n";
    asmf << "abss_slow PROC\n  push rcx\n  push rdx\n  push r8\n  push r9\n  sub rsp, 104\n"
            "  movdqu xmmword ptr [rsp+32], xmm0\n  movdqu xmmword ptr [rsp+48], xmm1\n"
            "  movdqu xmmword ptr [rsp+64], xmm2\n  movdqu xmmword ptr [rsp+80], xmm3\n"
            "  mov ecx, r10d\n  call abss_resolve\n"
            "  movdqu xmm0, xmmword ptr [rsp+32]\n  movdqu xmm1, xmmword ptr [rsp+48]\n"
            "  movdqu xmm2, xmmword ptr [rsp+64]\n  movdqu xmm3, xmmword ptr [rsp+80]\n"
            "  add rsp, 104\n  pop r9\n  pop r8\n  pop rdx\n  pop rcx\n  jmp rax\nabss_slow ENDP\nEND\n";
    cf << "/* Generated by ABSS: resolves thunk targets in abss_abqstub.dll next to this DLL. */\n"
          "#include <windows.h>\nextern IMAGE_DOS_HEADER __ImageBase;\n"
       << "void* abss_fp[" << std::max<size_t>(thunked.size(), 1) << "];\n"
       << "static const char* const abss_names[] = {";
    for (const auto& n : thunked) cf << "\"" << n << "\",";
    cf << "0};\nstatic HMODULE abss_stub;\n"
          "void* abss_resolve(int i) {\n"
          "  if (!abss_stub) {\n"
          "    wchar_t p[4096]; DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, p, 4000);\n"
          "    while (n && p[n - 1] != L'\\\\') --n; p[n] = 0; lstrcatW(p, L\"abss_abqstub.dll\");\n"
          "    abss_stub = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);\n"
          "    if (!abss_stub) FatalAppExitA(0, \"ABSS: abss_abqstub.dll (Abaqus stub copy) could not be loaded\");\n"
          "  }\n"
          "  void* f = (void*)GetProcAddress(abss_stub, abss_names[i]);\n"
          "  if (!f) FatalAppExitA(0, \"ABSS: entry point missing in abss_abqstub.dll\");\n"
          "  abss_fp[i] = f;\n  return f;\n}\n";
    def << "LIBRARY standardU\nEXPORTS\n";
    for (const auto& n : mine) def << "  " << n << "\n";
    for (size_t i = 0; i < thunked.size(); ++i) def << "  " << thunked[i] << "=abss_t" << i << "\n";
    writeFile(dir / "abss_thunks.asm", asmf.str());
    writeFile(dir / "abss_resolver.c", cf.str());
    writeFile(dir / "standardU.def", def.str());
    tool("ml64.exe", {"/nologo", "/c", "abss_thunks.asm"}, dir, "Assembling thunks");
    tool("cl.exe", {"/nologo", "/c", "/O2", "/GS-", "abss_resolver.c"}, dir, "Compiling resolver");
    // 5. link with the same runtime choices as Abaqus' link_sl (dynamic Intel/MSVC runtimes)
    std::vector<std::string> la = {"/nologo", "/dll", "/noentry", "/machine:x64", "/incremental:no", "/nodefaultlib",
                                   "/out:standardU.dll", "/def:standardU.def", obj.filename().string(),
                                   "abss_thunks.obj", "abss_resolver.obj", "kernel32.lib", "user32.lib", "advapi32.lib",
                                   "msvcrt.lib", "vcruntime.lib", "ucrt.lib", "oldnames.lib"};
    if (fortran || object) {
        // Intel runtimes (present in every Abaqus installation: libifcoremd.dll, libmmd.dll)
        for (const char* l : {"libifcoremd.lib", "libifportmd.lib", "libmmd.lib"})
            if (detail::which(l) || !tc.fortranCompiler.empty()) la.push_back(l);
    }
    tool("link.exe", la, dir, "Linking standardU.dll");
}

}  // namespace

CompilerToolchain detectCompilerToolchain() {
    CompilerToolchain tc;
#ifdef _WIN32
    const std::string pf86 = envOr("ProgramFiles(x86)", "C:\\Program Files (x86)");
    const fs::path vswhere = fs::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (fs::exists(vswhere, ec)) {
        const auto r = detail::runProcess(vswhere.string(),
                                          {"-latest", "-products", "*", "-requires",
                                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                                           "installationPath"},
                                          "", 60, nullptr);
        const std::string path = trim(r.output);
        if (!path.empty()) {
            const fs::path v = fs::path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
            if (fs::exists(v, ec)) {
                tc.msvc = true;
                tc.vcvars = v.string();
            }
        }
    }
    for (const auto& root : {envOr("ONEAPI_ROOT", ""), pf86 + "\\Intel\\oneAPI", std::string("C:\\Program Files\\Intel\\oneAPI")}) {
        if (root.empty() || !fs::exists(root, ec)) continue;
        // unified layout (oneAPI >= 2024): <root>\<ver>\bin ; component layout: <root>\compiler\<ver>\bin
        std::vector<fs::path> bins;
        if (auto v = newestVersionDir(root); !v.empty()) bins.push_back(v / "bin");
        if (auto v = newestVersionDir(fs::path(root) / "compiler"); !v.empty()) bins.push_back(v / "bin");
        bins.push_back(fs::path(root) / "compiler" / "latest" / "bin");
        for (const char* comp : {"ifort", "ifx"}) {
            for (const auto& b : bins)
                if (fs::exists(b / (std::string(comp) + ".exe"), ec)) {
                    tc.intelFortran = true;
                    tc.fortranCompiler = comp;
                    tc.oneapiSetvars = (fs::path(root) / "setvars.bat").string();
                    break;
                }
            if (tc.intelFortran) break;
        }
        if (tc.intelFortran) break;
    }
    if (!tc.intelFortran) {
        if (detail::which("ifort.exe")) tc.intelFortran = true, tc.fortranCompiler = "ifort";
        else if (detail::which("ifx.exe")) tc.intelFortran = true, tc.fortranCompiler = "ifx";
    }
    if (!tc.msvc && detail::which("link.exe")) tc.msvc = true;
    std::ostringstream m;
    m << "Microsoft C++ linker: " << (tc.msvc ? "found" : "not found") << "; Intel Fortran: "
      << (tc.intelFortran ? tc.fortranCompiler : std::string("not found"));
    if (!tc.msvc || !tc.intelFortran)
        m << ". Prebuilt standardU.dll libraries still work; to build from source or .obj install the compilers "
             "(ABSS setup: 'User-subroutine compilers', or scripts\\setup_dependencies.ps1 -UserSubroutines).";
    tc.message = m.str();
#else
    tc.msvc = true;
    if (detail::which("ifort")) tc.intelFortran = true, tc.fortranCompiler = "ifort";
    else if (detail::which("ifx")) tc.intelFortran = true, tc.fortranCompiler = "ifx";
    else if (detail::which("gfortran")) tc.intelFortran = true, tc.fortranCompiler = "gfortran";
    tc.active = true;
    tc.message = tc.intelFortran ? "Fortran compiler: " + tc.fortranCompiler : "No Fortran compiler found";
#endif
    return tc;
}

bool activateCompilerToolchain(CompilerToolchain& tc) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(gEnvMutex);
    if (tc.active) return true;
    if (!tc.msvc) return false;
    // The environment is process-wide: load it once. Re-running vcvars64 on an already-extended PATH can exceed
    // cmd's line limit and fail.
    static bool processActivated = false;
    if (processActivated) {
        tc.active = detail::which("link.exe").has_value();
        if (detail::which("ifort.exe")) tc.fortranCompiler = "ifort";
        else if (detail::which("ifx.exe")) tc.fortranCompiler = "ifx";
        return tc.active;
    }
    // vcvars64.bat is reliable; Intel's setvars.bat is not (it fails under paths with parentheses, e.g.
    // "Program Files (x86)"), so the oneAPI directories are added explicitly below.
    if (!tc.vcvars.empty()) {
        const fs::path tmp = fs::temp_directory_path() / "abss_toolchain_env.bat";
        writeFile(tmp, "@echo off\r\ncall \"" + tc.vcvars + "\" >nul 2>&1\r\necho ABSS_ENV_BEGIN\r\nset\r\n");
        const auto r = detail::runProcess(tmp.string(), {}, fs::temp_directory_path().string(), 300, nullptr);
        std::error_code ec;
        fs::remove(tmp, ec);
        const auto pos = r.output.find("ABSS_ENV_BEGIN");
        if (pos == std::string::npos) {
            tc.message += " (could not load the Visual Studio environment)";
            return false;
        }
        std::istringstream in(r.output.substr(pos));
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            line = trim(line);
            const auto eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            SetEnvironmentVariableA(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str());
            _putenv_s(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str());
        }
    }
    if (!tc.oneapiSetvars.empty()) {
        const fs::path root = fs::path(tc.oneapiSetvars).parent_path();
        std::vector<fs::path> verDirs;
        if (auto v = newestVersionDir(root); !v.empty()) verDirs.push_back(v);
        if (auto v = newestVersionDir(root / "compiler"); !v.empty()) verDirs.push_back(v);
        auto prepend = [](const char* var, const fs::path& p) {
            std::error_code ec;
            if (!fs::exists(p, ec)) return;
            const std::string v = p.string() + ";" + envOr(var, "");
            SetEnvironmentVariableA(var, v.c_str());
            _putenv_s(var, v.c_str());
        };
        for (auto it = verDirs.rbegin(); it != verDirs.rend(); ++it) {
            prepend("PATH", *it / "bin");
            prepend("LIB", *it / "lib");
            prepend("INCLUDE", *it / "include");
        }
    }
    tc.active = detail::which("link.exe").has_value();
    processActivated = tc.active;
    if (detail::which("ifort.exe")) tc.fortranCompiler = "ifort";
    else if (detail::which("ifx.exe")) tc.fortranCompiler = "ifx";
    return tc.active;
#else
    tc.active = true;
    return true;
#endif
}

UserSubroutineLibrary prepareUserSubroutine(const std::string& abaqusCommand, const std::string& input,
                                            const std::string& buildDir, const std::function<void(const std::string&)>& log) {
    UserSubroutineLibrary lib;
    std::error_code ec;
    const fs::path in = fs::absolute(fs::path(input), ec);
    if (!fs::exists(in, ec)) throw std::runtime_error("The user subroutine was not found: " + input);
#ifdef _WIN32
    const char* libName = "standardU.dll";
#else
    const char* libName = "libstandardU.so";
#endif
    // 0) Foreign material library (Linux .so, or a .dll that is not standardU.dll): material bridge. A folder without
    //    standardU.dll that holds exactly one such library (directly or in lib/) is accepted too (a vendor package).
    fs::path foreign;
    if (detail::isForeignMaterialLibrary(in)) foreign = in;
    if (fs::is_directory(in, ec) && !fs::exists(in / libName, ec)) {
        std::vector<fs::path> found;
        for (const auto& d : {in, in / "lib"})
            for (const auto& e : fs::directory_iterator(d, ec))
                if (detail::isForeignMaterialLibrary(e.path())) found.push_back(e.path());
        if (found.size() == 1) foreign = found.front();
    }
    if (!foreign.empty()) {
#ifdef _WIN32
        auto tc = detectCompilerToolchain();
        if (!tc.msvc)
            throw std::runtime_error("Running " + foreign.filename().string() + " through the material bridge needs the "
                                     "Microsoft C++ tools to build the bridge UMAT once (ABSS setup: 'User-subroutine "
                                     "compilers'). (" + tc.message + ")");
        if (!activateCompilerToolchain(tc))
            throw std::runtime_error("The compiler environment could not be loaded (" + tc.message + ").");
        const fs::path home = abaqusHome(abaqusCommand);
        if (home.empty()) throw std::runtime_error("Could not determine the Abaqus installation directory.");
        const fs::path dir = fs::absolute(buildDir);
        if (log) log("Building the material bridge for " + foreign.filename().string() + "...");
        return detail::prepareMaterialBridge(
            home, foreign, dir, [&](const fs::path& src) { directBuild(home, src, false, false, dir, tc, log); },
            [](const fs::path& p) { return peExportNames(p); }, log);
#else
        if (log) log("Building the material bridge for " + foreign.filename().string() + "...");
        return detail::prepareMaterialBridgePosix(abaqusCommand, foreign, fs::absolute(buildDir), log);
#endif
    }
    // 1) Prebuilt library: folder containing it, or the library file itself. Nothing to compile.
    if (fs::is_directory(in, ec) || lower(in.filename().string()) == lower(libName)) {
        const fs::path dir = fs::is_directory(in, ec) ? in : in.parent_path();
        if (!fs::exists(dir / libName, ec))
            throw std::runtime_error("The folder " + dir.string() + " does not contain " + libName +
                                     " (build one with ABSS or `abaqus make library=<file>`).");
        lib.libraryDir = dir.string();
        lib.kind = "prebuilt library";
        lib.message = "Using prebuilt user-subroutine library " + (dir / libName).string() + " (no compiler needed).";
        if (log) log(lib.message);
        return lib;
    }
    const std::string ext = lower(in.extension().string());
    const bool object = ext == ".obj" || ext == ".o";
    const bool fortran = ext == ".f" || ext == ".for" || ext == ".f90" || ext == ".f77";
    const bool csrc = ext == ".c" || ext == ".cpp" || ext == ".cxx" || ext == ".cc";
    if (!object && !fortran && !csrc)
        throw std::runtime_error("Unrecognised user-subroutine file type '" + ext +
                                 "'. Use a folder/standardU.dll, an object file (.obj), or source (.f, .for, .f90, .c, .cpp).");
    lib.kind = object ? "object file" : fortran ? "Fortran source" : "C/C++ source";

    // 2) Object or source: build standardU.dll once.
    auto tc = detectCompilerToolchain();
    if (!tc.msvc || (fortran && !tc.intelFortran))
        throw std::runtime_error("Building this " + lib.kind + " needs " +
                                 (fortran ? std::string("Intel Fortran and the Microsoft C++ tools") : std::string("the Microsoft C++ tools")) +
                                 ", which were not found. Install them with the ABSS setup ('User-subroutine compilers') or "
                                 "scripts\\setup_dependencies.ps1 -UserSubroutines, or supply a prebuilt standardU.dll. (" +
                                 tc.message + ")");
    if (!activateCompilerToolchain(tc))
        throw std::runtime_error("The compiler environment could not be loaded (" + tc.message + ").");
    const fs::path dir = fs::absolute(buildDir);
    fs::create_directories(dir);
    const fs::path home = abaqusHome(abaqusCommand);
    const bool devFiles = !home.empty() && fs::exists(home / "win_b64" / "SMA" / "site" / "aba_param.inc", ec);
    if (devFiles) {
        const fs::path src = dir / in.filename();
        fs::copy_file(in, src, fs::copy_options::overwrite_existing, ec);
        if (tc.fortranCompiler == "ifx") {
            // Abaqus 2024 calls `ifort`, which Intel no longer ships (oneAPI >= 2025). Same flags, minus ifort-only ones.
            writeFile(dir / "abaqus_v6.env",
                      "# Written by ABSS: compile Abaqus user subroutines with Intel ifx (ifort is no longer distributed).\n"
                      "import os\n"
                      "_abss_inc = os.path.abspath(os.path.join(os.environ.get('ABA_HOME', ''), os.pardir))\n"
                      "compile_fortran = ['ifx', '/c', '/fpp', '/extend-source', '/DABQ_WIN86_64', '/DABQ_FORTRAN',\n"
                      "                   '/iface:cref', '/recursive', '/Qauto', '/align:array64byte', '/Qprec-div',\n"
                      "                   '/Qfma-', '/fp:precise', '/reentrancy:threaded',\n"
                      "                   '/include:%I', '/include:' + _abss_inc, '%P']\n"
                      "del _abss_inc\n");
        }
        if (log) log("Building the user-subroutine library once with `abaqus make` (" + lib.kind + ")...");
        const auto r = detail::runProcess(abaqusCommand, {"make", "library=" + src.filename().string(), "directory=" + dir.string()},
                                          dir.string(), 1800, nullptr);
        if (!fs::exists(dir / libName, ec)) {
            std::string tail = r.output.substr(r.output.size() > 1500 ? r.output.size() - 1500 : 0);
            throw std::runtime_error("`abaqus make` did not produce " + std::string(libName) + " from " + in.filename().string() +
                                     ". Compiler/linker output:\n" + tail);
        }
    } else {
#ifdef _WIN32
        if (home.empty()) throw std::runtime_error("Could not determine the Abaqus installation directory.");
        if (log) log("Building the user-subroutine library once (direct build; " + lib.kind + ")...");
        directBuild(home, in, object, fortran, dir, tc, log);
#else
        throw std::runtime_error("This Abaqus installation lacks its user-subroutine development files (aba_param.inc).");
#endif
    }
    lib.libraryDir = dir.string();
    lib.builtHere = true;
    lib.message = "Built " + (dir / libName).string() + " from " + in.filename().string() + "; all jobs load it via usub_lib_dir.";
    if (log) log(lib.message);
    return lib;
}

}  // namespace abss
