// The launcher on Windows (platform.h). Processes with CreateProcessW, each in a job object so that a
// cancel ends a build's compilers too; HTTPS through WinHTTP; the disc chooser is the system's
// IFileOpenDialog. Paths go to Windows as UTF-16.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

#include "../build_strategy.h"
#include "../platform.h"

namespace launcher {

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}

fs::path program_dir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (n == 0)
            return fs::current_path();
        if (n < path.size()) {
            path.resize(n);
            return fs::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

// one argument as the C runtime reads it back (CommandLineToArgvW's rules)
void append_quoted(std::wstring &line, const std::wstring &arg) {
    if (!line.empty())
        line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (std::size_t i = 0;; i++) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            slashes++;
            i++;
        }
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            line.append(slashes * 2 + 1, L'\\');
            line += L'"';
        } else {
            line.append(slashes, L'\\');
            line += arg[i];
        }
    }
    line += L'"';
}

// the launcher's environment with `extra` set on top, as CreateProcessW takes it (sorted is not needed)
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>> &extra) {
    std::wstring block;
    wchar_t *own = GetEnvironmentStringsW();
    for (const wchar_t *e = own; e != nullptr && *e != 0; e += wcslen(e) + 1) {
        std::wstring entry = e;
        bool replaced = false;
        for (const auto &[name, value] : extra) {
            std::wstring prefix = widen(name) + L"=";
            replaced |= _wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0;
        }
        if (!replaced) {
            block += entry;
            block += L'\0';
        }
    }
    if (own != nullptr)
        FreeEnvironmentStringsW(own);
    for (const auto &[name, value] : extra) {
        block += widen(name) + L"=" + widen(value);
        block += L'\0';
    }
    block += L'\0';
    return block;
}

class WindowsProcess final : public ChildProcess {
public:
    WindowsProcess(HANDLE process, HANDLE job, HANDLE output) : process_(process), job_(job), output_(output) {}
    ~WindowsProcess() override {
        if (output_ != nullptr)
            CloseHandle(output_);
        if (job_ != nullptr)
            CloseHandle(job_);
        CloseHandle(process_);
    }

    std::optional<std::string> read_line() override {
        for (;;) {
            std::size_t end = pending_.find('\n');
            if (end != std::string::npos) {
                std::string line = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line;
            }
            if (output_ == nullptr) {
                if (pending_.empty())
                    return std::nullopt;
                std::string line;
                line.swap(pending_);
                return line;
            }
            char buffer[4096];
            DWORD got = 0;
            if (!ReadFile(output_, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
                CloseHandle(output_);
                output_ = nullptr;
                continue;
            }
            pending_.append(buffer, got);
        }
    }

    void stop() override {
        if (job_ != nullptr)
            TerminateJobObject(job_, 1);
        else
            TerminateProcess(process_, 1);
    }

    int wait() override {
        if (WaitForSingleObject(process_, INFINITE) != WAIT_OBJECT_0)
            return -1;
        DWORD code = 0;
        if (!GetExitCodeProcess(process_, &code))
            return -1;
        return (int)code;
    }

private:
    HANDLE process_;
    HANDLE job_;
    HANDLE output_;
    std::string pending_;
};

constexpr std::size_t kMaxAnswer = 1 << 20;

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }
};

class WindowsPlatform final : public Platform {
public:
    std::string_view name() const override { return "windows"; }

    fs::path content_dir() const override {
        if (std::string given = env_or_empty("KF4_CONTENT_DIR"); !given.empty())
            return fs::u8path(given);
        return program_dir() / "content";
    }

    std::optional<Toolchain> toolchain(const fs::path &content, std::string &why) const override {
        fs::path root = content / "toolchain";
        Toolchain tools;
        tools.cmake = root / "cmake/bin/cmake.exe";
        tools.ninja = root / "ninja/ninja.exe";
        tools.c_compiler = root / "llvm-mingw/bin/clang.exe";
        tools.cxx_compiler = root / "llvm-mingw/bin/clang++.exe";
        fs::path sdl2 = root / "SDL2/x86_64-w64-mingw32/lib/cmake/SDL2";
        std::error_code error;
        for (const fs::path &tool : {tools.cmake, tools.ninja, tools.c_compiler, tools.cxx_compiler, sdl2}) {
            if (!fs::exists(tool, error)) {
                why = tool.u8string() + " is missing";
                return std::nullopt;
            }
        }
        tools.cmake_options = {"-DSDL2_DIR=" + sdl2.generic_u8string()};
        tools.env = {{"PATH", (root / "llvm-mingw/bin").u8string() + ";" + env_or_empty("PATH")}};
        auto id = std::fopen((root / "ID").u8string().c_str(), "rb");
        char buffer[256] = {};
        if (id != nullptr) {
            std::fread(buffer, 1, sizeof(buffer) - 1, id);
            std::fclose(id);
        }
        tools.id = std::string("windows:") + buffer;
        return tools;
    }

    std::string_view game_program() const override { return "kf4.exe"; }

    std::vector<fs::path> game_files(const fs::path &build_dir) const override {
        std::vector<fs::path> files;
        std::error_code error;
        if (!fs::is_regular_file(build_dir / "kf4.exe", error))
            return files;
        files.push_back(build_dir / "kf4.exe");
        // its DLLs, copied beside it by the build (CMakeLists.txt: SDL2, libc++, libunwind)
        for (fs::directory_iterator it(build_dir, error), end; !error && it != end; it.increment(error))
            if (it->path().extension() == ".dll")
                files.push_back(it->path());
        return files;
    }

    const BuildStrategy &build_strategy() const override { return local_cmake_build(); }

    std::unique_ptr<ChildProcess> start(const ProcessSpec &spec, std::string &why) const override {
        std::wstring line;
        for (const std::string &arg : spec.argv)
            append_quoted(line, widen(arg));
        std::wstring env = environment_block(spec.env);
        std::wstring dir = spec.working_dir.empty() ? std::wstring() : spec.working_dir.wstring();

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read_end = nullptr, write_end = nullptr;
        if (spec.capture_output) {
            if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
                why = "CreatePipe failed (" + std::to_string(GetLastError()) + ")";
                return nullptr;
            }
            SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (spec.capture_output) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = write_end;
            startup.hStdError = write_end;
        }
        // a captured program has no console of its own: no black window over the launcher
        DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (spec.capture_output ? CREATE_NO_WINDOW : 0);
        PROCESS_INFORMATION info{};
        BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, spec.capture_output, flags, env.data(),
                                      dir.empty() ? nullptr : dir.c_str(), &startup, &info);
        if (write_end != nullptr)
            CloseHandle(write_end);
        if (!started) {
            why = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
            if (read_end != nullptr)
                CloseHandle(read_end);
            return nullptr;
        }
        HANDLE job = nullptr;
        if (spec.stop_with_children) {
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job != nullptr && (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                                   !AssignProcessToJobObject(job, info.hProcess))) {
                CloseHandle(job);
                job = nullptr;
            }
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        return std::make_unique<WindowsProcess>(info.hProcess, job, read_end);
    }

    std::optional<fs::path> pick_disc() const override {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::optional<fs::path> chosen;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog,
                                       reinterpret_cast<void **>(&dialog)))) {
            COMDLG_FILTERSPEC filters[] = {{L"King's Field IV disc (.iso, or an extracted disc's SYSTEM.CNF)", L"*.iso;SYSTEM.CNF"},
                                           {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, filters);
            dialog->SetTitle(L"Choose your King's Field IV disc");
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    chosen = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return chosen;
    }

    std::optional<std::string> http_get(std::string_view url, std::string_view user_agent,
                                        int timeout_ms) const override {
        std::wstring wide_url = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {}, path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
            return std::nullopt;
        InternetHandle session{WinHttpOpen(widen(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (session.handle == nullptr)
            return std::nullopt;
        WinHttpSetTimeouts(session.handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        InternetHandle connection{WinHttpConnect(session.handle, host, parts.nPort, 0)};
        if (connection.handle == nullptr)
            return std::nullopt;
        InternetHandle request{WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (request.handle == nullptr ||
            !WinHttpSendRequest(request.handle, L"Accept: application/vnd.github+json", (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.handle, nullptr))
            return std::nullopt;
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
            status != 200)
            return std::nullopt;
        std::string answer;
        for (;;) {
            char buffer[8192];
            DWORD got = 0;
            if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &got))
                return std::nullopt;
            if (got == 0)
                break;
            if (answer.size() + got > kMaxAnswer)
                return std::nullopt;
            answer.append(buffer, got);
        }
        return answer;
    }

    void attach_console() const override {
        // started from a console (cmd, PowerShell): print there; from Explorer there is none
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *ignored = nullptr;
            freopen_s(&ignored, "CONOUT$", "w", stdout);
            freopen_s(&ignored, "CONOUT$", "w", stderr);
        }
    }
};

} // namespace

const Platform &this_platform() {
    static const WindowsPlatform platform;
    return platform;
}

std::string env_or_empty(const char *name) {
    std::wstring wide = widen(name);
    DWORD n = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (n == 0)
        return {};
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wide.c_str(), value.data(), n);
    value.resize(n);
    return fs::path(value).u8string();
}

} // namespace launcher
