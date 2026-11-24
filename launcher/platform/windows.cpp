// The launcher on Windows (platform.h). Processes with CreateProcessW, each in a job object so that a
// cancel ends a build's compilers too; HTTPS through WinHTTP; the disc chooser is the system's
// IFileOpenDialog. Paths go to Windows as UTF-16.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <system_error>

#include "../build_strategy.h"
#include "../launcher_files.h"
#include "../platform.h"
#include "../update_delta.h"

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

// the launcher's own .exe; empty if Windows does not say
fs::path program_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            return fs::path(path);
        }
        path.resize(path.size() * 2);
    }
}

fs::path program_dir() {
    fs::path self = program_path();
    return self.empty() ? fs::current_path() : self.parent_path();
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

// What Windows' exception codes, ended with, mean. Some crashes cannot run the game's own crash report (a stack
// overflow, a fast fail): this line in game.log is then all there is.
struct ExitMeaning {
    DWORD code;
    const char *meaning;
};
constexpr ExitMeaning kExitMeanings[] = {
    {0xC0000005, "an access violation (a read or write of memory it does not have)"},
    {0xC00000FD, "a stack overflow"},
    {0xC0000409, "a fast fail (a failed security check or a corrupted heap: no crash report is possible)"},
    {0xC0000374, "a corrupted heap"},
    {0xC000001D, "an illegal instruction (one this CPU does not have)"},
    {0xC0000094, "an integer divided by zero"},
    {0xC0000096, "a privileged instruction"},
    {0xC0000017, "no memory left"},
    {0x80000003, "a breakpoint"},
    {0x00000003, "abort() (a failed assertion, or a C++ error nothing caught)"},
    {PORT_EXIT_GPU_LOST, kGpuLostMeaning},
};

constexpr std::size_t kMaxAnswer = 1 << 20;

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }
};

// A GET's request, sent and answered 200
struct OpenGet {
    InternetHandle session, connection, request;
};
std::string open_get(OpenGet &get, std::string_view url, std::string_view user_agent, int timeout_ms,
                     const wchar_t *headers) {
    std::wstring wide_url = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return "not an https:// address";
    get.session.handle = WinHttpOpen(widen(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (get.session.handle == nullptr)
        return "WinHTTP did not start (" + std::to_string(GetLastError()) + ")";
    WinHttpSetTimeouts(get.session.handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    get.connection.handle = WinHttpConnect(get.session.handle, host, parts.nPort, 0);
    if (get.connection.handle == nullptr)
        return "cannot reach the server (" + std::to_string(GetLastError()) + ")";
    // (redirects are followed: GitHub sends a release's files to its file host)
    get.request.handle = WinHttpOpenRequest(get.connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (get.request.handle == nullptr ||
        !WinHttpSendRequest(get.request.handle, headers, headers != nullptr ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(get.request.handle, nullptr))
        return "no answer (" + std::to_string(GetLastError()) + ")";
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(get.request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return "no answer";
    if (status != 200)
        return "the server answered " + std::to_string(status);
    return {};
}

// a program started on its own, in no job of the launcher's, so that it outlives the launcher
std::string start_on_its_own(const fs::path &program, const std::vector<std::wstring> &args) {
    std::wstring line;
    append_quoted(line, program.wstring());
    for (const std::wstring &arg : args)
        append_quoted(line, arg);
    std::wstring dir = program.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                        dir.c_str(), &startup, &info))
        return "cannot start " + program.u8string() + " (" + std::to_string(GetLastError()) + ")";
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return {};
}

// a standard stream redirected to a file or a pipe (`holyland > out.txt`, `holyland | more`); a window program started
// from a console without redirection has none, or the console's own
bool redirected(DWORD which) {
    HANDLE handle = GetStdHandle(which);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

fs::path with_old(const fs::path &file) {
    fs::path old = file;
    old += ".old";
    return old;
}

// a running program or a DLL it has loaded can be renamed, not written over or deleted
bool move_aside(const fs::path &file) {
    std::error_code error;
    if (!fs::exists(file, error))
        return true;
    fs::remove_all(with_old(file), error);
    return move_replacing(file, with_old(file));
}

void move_back(const fs::path &file) {
    std::error_code error;
    if (fs::exists(with_old(file), error)) {
        fs::remove_all(file, error);
        move_replacing(with_old(file), file);
    }
}

// The files beside the launcher that it has open while it runs
std::vector<fs::path> running_files(const fs::path &dir, const fs::path &exe) {
    return {exe, dir / "SDL3.dll"};
}

// Installed by the installer (packaging/windows/installer.iss; its uninstaller is beside the launcher): the new
// release's installer runs silently over this one, and starts the new launcher when it is done.
class InstallerInstallation final : public Installation {
public:
    InstallerInstallation(fs::path dir, fs::path exe) : dir_(std::move(dir)), exe_(std::move(exe)) {}

    std::string_view asset_suffix() const override { return "-win-x64-setup.exe"; }
    fs::path download_path() const override { return dir_ / "update-setup.exe"; }

    std::string replace_and_start(const fs::path &downloaded, const std::vector<std::string> &) const override {
        for (const fs::path &file : running_files(dir_, exe_)) {
            if (!move_aside(file)) {
                for (const fs::path &back : running_files(dir_, exe_))
                    move_back(back);
                return "cannot move " + file.u8string() + " aside (" + std::to_string(GetLastError()) + ")";
            }
        }
        // installer.iss starts the new launcher after a silent install only when asked: /HOLYLANDRELAUNCH
        std::string why = start_on_its_own(downloaded, {L"/VERYSILENT", L"/SUPPRESSMSGBOXES", L"/NORESTART",
                                                         L"/HOLYLANDRELAUNCH"});
        if (!why.empty()) {
            for (const fs::path &file : running_files(dir_, exe_))
                move_back(file);
        }
        return why;
    }

    void remove_leftovers() const override {
        std::error_code error;
        for (const fs::path &file : running_files(dir_, exe_))
            fs::remove(with_old(file), error);
        fs::remove(download_path(), error);
        fs::path sums = download_path();
        sums += ".sums";
        fs::remove(sums, error);
    }

private:
    fs::path dir_, exe_;
};

// Unpacked from the zip: the new zip is unpacked beside, with Windows' own tar.exe (Windows 10 1803 and later), and
// each of its files and folders put in place of the old ones, which are moved aside first.
class ZipInstallation final : public Installation {
public:
    ZipInstallation(fs::path dir, fs::path exe) : dir_(std::move(dir)), exe_(std::move(exe)) {}

    std::string_view asset_suffix() const override { return "-win-x64.zip"; }
    fs::path download_path() const override { return dir_ / "update.zip"; }

    // `downloaded`: the release's zip, or the folder made from its delta (made_from_delta), already whole
    std::string replace_and_start(const fs::path &downloaded, const std::vector<std::string> &args) const override {
        std::error_code error;
        fs::path unpacked = staged_path();
        std::string why;
        if (!fs::is_directory(downloaded, error)) {
            why = unpack_fresh(downloaded, unpacked);
            if (!why.empty())
                return why;
        }
        // the new files: what the zip holds at its top
        std::vector<fs::path> names;
        for (fs::directory_iterator it(unpacked, error), end; !error && it != end; it.increment(error))
            names.push_back(it->path().filename());
        if (error || !fs::is_regular_file(unpacked / exe_.filename(), error)) {
            fs::remove_all(unpacked, error);
            return "the zip does not hold " + exe_.filename().u8string();
        }
        std::vector<fs::path> moved;
        for (const fs::path &name : names) {
            if (!move_aside(dir_ / name) ||
                !move_replacing(unpacked / name, dir_ / name)) {
                why = "cannot put " + name.u8string() + " in place (" + std::to_string(GetLastError()) + ")";
                moved.push_back(name);
                for (const fs::path &back : moved)
                    move_back(dir_ / back);
                fs::remove_all(unpacked, error);
                return why;
            }
            moved.push_back(name);
        }
        fs::remove_all(unpacked, error);
        std::vector<std::wstring> wide_args;
        for (const std::string &arg : args)
            wide_args.push_back(widen(arg));
        return start_on_its_own(exe_, wide_args);
    }

    void remove_leftovers() const override {
        std::error_code error;
        for (fs::directory_iterator it(dir_, error), end; !error && it != end; it.increment(error)) {
            if (it->path().extension() == ".old") {
                std::error_code ignored;
                fs::remove_all(it->path(), ignored);
            }
        }
        fs::remove_all(staged_path(), error);
        fs::remove(download_path(), error);
        fs::path sums = download_path();
        sums += ".sums";
        fs::remove(sums, error);
        fs::remove(delta_download_path(), error);
    }

    // a tree delta (update_delta.h): unpacked as the zip is, the unchanged files linked from this folder
    fs::path delta_download_path() const override { return dir_ / "update.delta"; }

    Made made_from_delta(const fs::path &delta, std::string_view) const override {
        fs::path staged = staged_path();
        std::string why = unpack_fresh(delta, staged);
        std::error_code error;
        fs::remove(delta, error);
        if (why.empty())
            why = complete_tree(staged, dir_);
        if (!why.empty()) {
            fs::remove_all(staged, error);
            return {{}, why};
        }
        return {staged, {}};
    }

private:
    fs::path staged_path() const { return dir_ / "update.partial"; }

    // `zip` unpacked into `into`, made empty first; "" or why not, with `into` removed
    static std::string unpack_fresh(const fs::path &zip, const fs::path &into) {
        std::error_code error;
        fs::remove_all(into, error);
        if (!fs::create_directory(into, error))
            return "cannot make " + into.u8string() + ": " + error.message();
        std::string why = unpack(zip, into);
        if (!why.empty())
            fs::remove_all(into, error);
        return why;
    }

    static std::string unpack(const fs::path &zip, const fs::path &into) {
        std::wstring system(MAX_PATH, L'\0');
        UINT n = GetSystemDirectoryW(system.data(), (UINT)system.size());
        if (n == 0 || n >= system.size())
            return "cannot find Windows' folder";
        system.resize(n);
        fs::path tar = fs::path(system) / "tar.exe";
        std::error_code error;
        if (!fs::is_regular_file(tar, error))
            return "this Windows has no tar.exe (Windows 10 1803 or later has it): use the download page";
        std::wstring line;
        for (const std::wstring &arg : {tar.wstring(), std::wstring(L"-xf"), zip.wstring(), std::wstring(L"-C"),
                                        into.wstring()})
            append_quoted(line, arg);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &info))
            return "cannot start tar.exe (" + std::to_string(GetLastError()) + ")";
        CloseHandle(info.hThread);
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        if (code != 0)
            return "the zip could not be unpacked (tar.exe ended with " + std::to_string(code) + ")";
        return {};
    }

    fs::path dir_, exe_;
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
        fs::path sdl3 = root / "SDL3/x86_64-w64-mingw32/lib/cmake/SDL3";
        std::error_code error;
        for (const fs::path &tool : {tools.cmake, tools.ninja, tools.c_compiler, tools.cxx_compiler, sdl3}) {
            if (!fs::exists(tool, error)) {
                why = tool.u8string() + " is missing";
                return std::nullopt;
            }
        }
        tools.cmake_options = {"-DSDL3_DIR=" + sdl3.generic_u8string()};
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
        // its DLLs, copied beside it by the build (CMakeLists.txt: SDL3, libc++, libunwind)
        for (fs::directory_iterator it(build_dir, error), end; !error && it != end; it.increment(error))
            if (it->path().extension() == ".dll")
                files.push_back(it->path());
        return files;
    }

    const BuildStrategy &build_strategy() const override { return local_cmake_build(); }

    std::string describe_exit(int code) const override {
        char text[160];
        std::snprintf(text, sizeof(text), "code 0x%08lX", (unsigned long)(DWORD)code);
        for (const ExitMeaning &known : kExitMeanings) {
            if (known.code == (DWORD)code)
                return std::string(text) + ", " + known.meaning;
        }
        return text;
    }

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
        // started suspended so it is in the job before it can start children of its own
        if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
            why = "ResumeThread failed (" + std::to_string(GetLastError()) + ")";
            TerminateProcess(info.hProcess, 1);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            if (job != nullptr)
                CloseHandle(job);
            if (read_end != nullptr)
                CloseHandle(read_end);
            return nullptr;
        }
        CloseHandle(info.hThread);
        return std::make_unique<WindowsProcess>(info.hProcess, job, read_end);
    }

    std::optional<fs::path> pick_disc() const override {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::optional<fs::path> chosen;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog,
                                       reinterpret_cast<void **>(&dialog)))) {
            COMDLG_FILTERSPEC filters[] = {{L"King's Field IV disc (.iso, .chd, or an extracted disc's SYSTEM.CNF)", L"*.iso;*.chd;SYSTEM.CNF"},
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
        OpenGet get;
        if (!open_get(get, url, user_agent, timeout_ms, L"Accept: application/vnd.github+json").empty())
            return std::nullopt;
        std::string answer;
        for (;;) {
            char buffer[8192];
            DWORD got = 0;
            if (!WinHttpReadData(get.request.handle, buffer, sizeof(buffer), &got))
                return std::nullopt;
            if (got == 0)
                break;
            if (answer.size() + got > kMaxAnswer)
                return std::nullopt;
            answer.append(buffer, got);
        }
        return answer;
    }

    std::string http_download(std::string_view url, const fs::path &to, std::uint64_t max_bytes,
                              std::string_view user_agent, const DownloadProgress &progress) const override {
        OpenGet get;
        std::string why = open_get(get, url, user_agent, 30000, nullptr);
        if (!why.empty())
            return why;
        std::uint64_t total = 0;
        wchar_t length[32] = {};
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(get.request.handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                &length_size, WINHTTP_NO_HEADER_INDEX))
            total = std::wcstoull(length, nullptr, 10);
        if (total > max_bytes)
            return "the file is larger than expected";
        std::FILE *file = _wfopen(to.c_str(), L"wb");
        if (file == nullptr)
            return "cannot write " + to.u8string();
        std::uint64_t done = 0;
        std::vector<char> buffer(1 << 16);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(get.request.handle, buffer.data(), (DWORD)buffer.size(), &got)) {
                why = "the connection broke (" + std::to_string(GetLastError()) + ")";
                break;
            }
            if (got == 0)
                break;
            if (done + got > max_bytes) {
                why = "the file is larger than expected";
                break;
            }
            if (std::fwrite(buffer.data(), 1, got, file) != got) {
                why = "cannot write " + to.u8string();
                break;
            }
            done += got;
            if (progress && !progress(done, total)) {
                why = "stopped";
                break;
            }
        }
        if (std::fclose(file) != 0 && why.empty())
            why = "cannot write " + to.u8string();
        if (why.empty() && total != 0 && done != total)
            why = "the download ended early";
        if (!why.empty()) {
            std::error_code error;
            fs::remove(to, error);
        }
        return why;
    }

    std::unique_ptr<Installation> installation(std::string &why) const override {
        fs::path exe = program_path();
        fs::path dir = exe.parent_path();
        std::error_code error;
        // a release's folder: the launcher and content/ with its manifest (a developer's build has neither)
        if (!fs::is_regular_file(exe, error) || !fs::is_regular_file(dir / "content" / "manifest.txt", error)) {
            why = "this is not an installed release";
            return nullptr;
        }
        if (fs::is_regular_file(dir / "unins000.exe", error))
            return std::make_unique<InstallerInstallation>(dir, exe);
        // a zip's folder must be one the launcher may write
        fs::path probe = dir / "update.write-test";
        std::FILE *f = _wfopen(probe.c_str(), L"wb");
        if (f == nullptr) {
            why = dir.u8string() + " cannot be written";
            return nullptr;
        }
        std::fclose(f);
        fs::remove(probe, error);
        return std::make_unique<ZipInstallation>(dir, exe);
    }

    void attach_console() const override {
        // started from a console (cmd, PowerShell): print there; from Explorer there is none. A stream the console
        // sent to a file or a pipe (`holyland > out.txt`, `2> err.txt`) is kept there.
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *ignored = nullptr;
            if (!redirected(STD_OUTPUT_HANDLE))
                freopen_s(&ignored, "CONOUT$", "w", stdout);
            if (!redirected(STD_ERROR_HANDLE))
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
