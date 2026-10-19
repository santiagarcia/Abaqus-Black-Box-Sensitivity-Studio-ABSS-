#include "abaqus_process.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace abss::detail {

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// Quote one argument for CommandLineToArgvW / cmd.exe consumption.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"&|<>^") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') q += L"\\\"";
        else q += c;
    }
    return q + L"\"";
}
}  // namespace

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
                         int timeoutSeconds, std::atomic<bool>* cancel) {
    ProcessResult res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return res;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    const bool isBatch = program.size() > 4 && (program.substr(program.size() - 4) == ".bat" || program.substr(program.size() - 4) == ".BAT" ||
                                                program.substr(program.size() - 4) == ".cmd");
    if (isBatch) {
        // cmd.exe /S /C ""prog" args" : the outer quotes are stripped by /S.
        cmd = L"cmd.exe /S /C \"" + quoteArg(widen(program));
        for (const auto& a : args) cmd += L" " + quoteArg(widen(a));
        cmd += L"\"";
    } else {
        cmd = quoteArg(widen(program));
        for (const auto& a : args) cmd += L" " + quoteArg(widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // A job object lets us terminate exactly the tree we started (never unrelated Abaqus processes).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    std::wstring wcwd = widen(cwd);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (job) CloseHandle(job);
        res.output = "Could not start: " + program;
        return res;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(wr);

    const auto start = std::chrono::steady_clock::now();
    char chunk[4096];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) || got == 0) break;
            res.output.append(chunk, got);
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        const bool cancelNow = cancel && cancel->load();
        const bool timeout = timeoutSeconds > 0 &&
                             std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds);
        if (cancelNow || timeout) {
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            res.cancelled = cancelNow;
            res.timedOut = timeout;
            break;
        }
    }
    DWORD got = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0) res.output.append(chunk, got);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    if (job) CloseHandle(job);
    return res;
}

std::optional<std::string> which(const std::string& exe) {
    wchar_t out[MAX_PATH];
    if (SearchPathW(nullptr, widen(exe).c_str(), nullptr, MAX_PATH, out, nullptr) > 0) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, out, -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, out, -1, s.data(), n, nullptr, nullptr);
        return s;
    }
    return std::nullopt;
}

#else

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& cwd,
                         int timeoutSeconds, std::atomic<bool>* cancel) {
    ProcessResult res;
    int fds[2];
    if (pipe(fds) != 0) return res;
    const pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    const auto start = std::chrono::steady_clock::now();
    char chunk[4096];
    int status = 0;
    for (;;) {
        ssize_t n;
        while ((n = read(fds[0], chunk, sizeof chunk)) > 0) res.output.append(chunk, static_cast<size_t>(n));
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        const bool cancelNow = cancel && cancel->load();
        const bool timeout = timeoutSeconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds);
        if (cancelNow || timeout) {
            kill(-pid, SIGTERM);
            waitpid(pid, &status, 0);
            res.cancelled = cancelNow;
            res.timedOut = timeout;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof chunk)) > 0) res.output.append(chunk, static_cast<size_t>(n));
    close(fds[0]);
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

std::optional<std::string> which(const std::string& exe) {
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
        size_t s = 0;
        while (s <= p.size()) {
            const size_t e = p.find(':', s);
            const fs::path cand = fs::path(p.substr(s, e == std::string::npos ? std::string::npos : e - s)) / exe;
            std::error_code ec;
            if (fs::exists(cand, ec)) return cand.string();
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
    return std::nullopt;
}
#endif

}  // namespace abss::detail
