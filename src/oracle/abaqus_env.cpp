#include "abss/oracle/abaqus.hpp"

#include "abaqus_process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace abss {

namespace {

std::vector<std::string> candidateCommands(const std::string& overrideCommand) {
    std::vector<std::string> c;
    if (!overrideCommand.empty()) c.push_back(overrideCommand);
    if (const char* env = std::getenv("ABSS_ABAQUS_CMD")) c.push_back(env);
#ifdef _WIN32
    for (const char* root : {"C:\\SIMULIA\\Commands", "C:\\Program Files\\SIMULIA\\Commands",
                             "D:\\SIMULIA\\Commands", "C:\\SIMULIA\\EstProducts\\Commands"})
        for (const char* name : {"abaqus.bat", "abq2025.bat", "abq2024.bat", "abq2023.bat", "abq2022.bat"})
            c.push_back(std::string(root) + "\\" + name);
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
        size_t s = 0;
        while (s <= p.size()) {
            const size_t e = p.find(';', s);
            const std::string dir = p.substr(s, e == std::string::npos ? std::string::npos : e - s);
            if (!dir.empty()) {
                c.push_back(dir + "\\abaqus.bat");
                c.push_back(dir + "\\abaqus.exe");
            }
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
#else
    c.push_back("/usr/SIMULIA/Commands/abaqus");
    c.push_back("/opt/SIMULIA/Commands/abaqus");
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
        size_t s = 0;
        while (s <= p.size()) {
            const size_t e = p.find(':', s);
            const std::string dir = p.substr(s, e == std::string::npos ? std::string::npos : e - s);
            if (!dir.empty()) c.push_back(dir + "/abaqus");
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
#endif
    return c;
}

}  // namespace

AbaqusInstallation detectAbaqus(const std::string& overrideCommand) {
    AbaqusInstallation inst;
    for (const auto& cand : candidateCommands(overrideCommand)) {
        std::error_code ec;
        if (cand.empty() || !fs::exists(cand, ec)) continue;
        inst.command = cand;
        break;
    }
    if (inst.command.empty()) {
        inst.message = "Abaqus was not found. Results can still be explored and the demo runs without it. "
                       "To run new analyses, install Abaqus or set the ABSS_ABAQUS_CMD environment variable.";
        return inst;
    }
    const auto tmp = fs::temp_directory_path() / "abss_probe";
    fs::create_directories(tmp);
    const auto res = detail::runProcess(inst.command, {"information=release"}, tmp.string(), 120, nullptr);
    std::smatch m;
    if (std::regex_search(res.output, m, std::regex(R"(Abaqus\s+(20\d\d|6\.\d+[-\w]*))"))) {
        inst.found = true;
        inst.version = "Abaqus " + m[1].str();
        inst.message = inst.version + " found at " + inst.command + ". Licence not checked yet.";
    } else {
        inst.message = "Found " + inst.command + " but it did not report a version (exit code " +
                       std::to_string(res.exitCode) + ").";
    }
    // Building user subroutines needs Intel Fortran + the Microsoft linker (prebuilt standardU.dll needs neither).
    const auto tc = detectCompilerToolchain();
    inst.userSubroutinesCompilable = tc.msvc && tc.intelFortran;
    return inst;
}

bool checkAbaqusLicense(AbaqusInstallation& inst, const std::string& scratchDir) {
    inst.licenseChecked = true;
    inst.licenseOk = false;
    if (!inst.found) return false;
    const fs::path dir = fs::path(scratchDir) / "licence_check";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "abss_lic.inp");
        f << "*HEADING\nABSS licence check (1 truss element)\n*NODE\n1, 0., 0.\n2, 1., 0.\n"
             "*ELEMENT, TYPE=T2D2, ELSET=B\n1, 1, 2\n*SOLID SECTION, ELSET=B, MATERIAL=M\n1.\n"
             "*MATERIAL, NAME=M\n*ELASTIC\n1000., 0.3\n*BOUNDARY\n1, 1, 2\n2, 2\n*STEP\n*STATIC\n"
             "*CLOAD\n2, 1, 1.\n*END STEP\n";
    }
    const auto res = detail::runProcess(inst.command, {"job=abss_lic", "interactive", "ask_delete=OFF", "cpus=1"},
                                        dir.string(), 600, nullptr);
    inst.licenseOk = res.output.find("COMPLETED") != std::string::npos;
    inst.message = inst.licenseOk ? inst.version + ": licence OK (test job completed)."
                                  : inst.version + ": the test job did not complete. Check the licence server. Details: " +
                                        res.output.substr(res.output.size() > 600 ? res.output.size() - 600 : 0);
    return inst.licenseOk;
}

}  // namespace abss
