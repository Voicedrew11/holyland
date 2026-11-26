// The launcher on macOS (platform.h, docs/port/MACOS.md). As Linux's (platform/linux.cpp, which this follows):
// processes with fork and exec, each build step in a process group of its own; HTTPS through the system's libcurl
// (macOS has /usr/lib/libcurl.4.dylib); the disc chooser is the system's own (osascript's choose file).
// No release ships macOS tools yet: the build uses Xcode's command line tools (clang) and Homebrew's cmake, ninja,
// glslang, sdl3 and molten-vk. Nor does it update itself yet (no .app is packaged).
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "../build_strategy.h"
#include "../platform.h"
#include "../update_delta.h"
#include "../product.h"

extern char **environ;

namespace launcher {

namespace {

fs::path program_dir() {
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0)
        return fs::current_path();
    std::error_code error;
    fs::path self = fs::canonical(path, error);
    return error ? fs::path(path).parent_path() : self.parent_path();
}

// where Homebrew puts its programs (a program started from Finder has a PATH without them)
const char *const kToolDirs[] = {"/opt/homebrew/bin", "/usr/local/bin"};

// a program on PATH, then in Homebrew's folders
std::optional<fs::path> on_path(std::string_view name) {
    std::string path = env_or_empty("PATH");
    for (const char *dir : kToolDirs)
        path += std::string(":") + dir;
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find(':', at);
        if (end == std::string::npos)
            end = path.size();
        fs::path candidate = fs::path(path.substr(at, end - at)) / name;
        if (end > at && access(candidate.c_str(), X_OK) == 0)
            return candidate;
        at = end + 1;
    }
    return std::nullopt;
}

class MacProcess final : public ChildProcess {
public:
    MacProcess(pid_t pid, int output) : pid_(pid), output_(output) {}
    ~MacProcess() override {
        if (output_ >= 0)
            close(output_);
        if (!waited_)
            wait();
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
            if (output_ < 0) {
                if (pending_.empty())
                    return std::nullopt;
                std::string line;
                line.swap(pending_);
                return line;
            }
            char buffer[4096];
            ssize_t got = read(output_, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0) {
                close(output_);
                output_ = -1;
                continue;
            }
            pending_.append(buffer, (std::size_t)got);
        }
    }

    void stop() override {
        // its group, the build's compilers with it; a process left in the launcher's group alone
        if (getpgid(pid_) == pid_) {
            kill(-pid_, SIGTERM);
            kill(-pid_, SIGKILL);
        } else {
            kill(pid_, SIGKILL);
        }
    }

    int wait() override {
        if (waited_)
            return code_;
        int status = 0;
        pid_t got;
        do {
            got = waitpid(pid_, &status, 0);
        } while (got < 0 && errno == EINTR);
        waited_ = true;
        if (got < 0)
            code_ = -1;
        else if (WIFEXITED(status))
            code_ = WEXITSTATUS(status);
        else
            code_ = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return code_;
    }

private:
    pid_t pid_;
    int output_;
    std::string pending_;
    bool waited_ = false;
    int code_ = -1;
};

// libcurl's numbers for what is used here (curl/curl.h; part of its ABI, never changed)
constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptHttpHeader = 10023;
constexpr int kOptFailOnError = 45, kOptFollowLocation = 52, kOptNoSignal = 99, kOptTimeoutMs = 155;
constexpr int kOptWriteFunction = 20011, kOptConnectTimeoutMs = 156, kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20;
constexpr int kInfoContentLengthDownloadT = 0x600000 + 15; // CURLINFO_OFF_T + 15
constexpr std::size_t kMaxAnswer = 1 << 20;

struct Curl {
    void *(*easy_init)();
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    void (*easy_cleanup)(void *);
    int (*easy_getinfo)(void *, int, ...);
    const char *(*easy_strerror)(int);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
};

const Curl *load_curl() {
    static Curl curl;
    static bool loaded = [] {
        void *lib = dlopen("/usr/lib/libcurl.4.dylib", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
            return false;
        curl.easy_init = reinterpret_cast<void *(*)()>(dlsym(lib, "curl_easy_init"));
        curl.easy_setopt = reinterpret_cast<int (*)(void *, int, ...)>(dlsym(lib, "curl_easy_setopt"));
        curl.easy_perform = reinterpret_cast<int (*)(void *)>(dlsym(lib, "curl_easy_perform"));
        curl.easy_cleanup = reinterpret_cast<void (*)(void *)>(dlsym(lib, "curl_easy_cleanup"));
        curl.easy_getinfo = reinterpret_cast<int (*)(void *, int, ...)>(dlsym(lib, "curl_easy_getinfo"));
        curl.easy_strerror = reinterpret_cast<const char *(*)(int)>(dlsym(lib, "curl_easy_strerror"));
        curl.slist_append = reinterpret_cast<void *(*)(void *, const char *)>(dlsym(lib, "curl_slist_append"));
        curl.slist_free_all = reinterpret_cast<void (*)(void *)>(dlsym(lib, "curl_slist_free_all"));
        return curl.easy_init && curl.easy_setopt && curl.easy_perform && curl.easy_cleanup && curl.easy_getinfo &&
               curl.easy_strerror && curl.slist_append && curl.slist_free_all;
    }();
    return loaded ? &curl : nullptr;
}

std::size_t keep_answer(char *data, std::size_t size, std::size_t count, void *context) {
    auto *answer = static_cast<std::string *>(context);
    std::size_t bytes = size * count;
    if (answer->size() + bytes > kMaxAnswer)
        return 0; // stops the transfer
    answer->append(data, bytes);
    return bytes;
}

// A download's file, written as curl hands the bytes over
struct DownloadTo {
    const Curl *curl;
    void *handle;
    std::FILE *file;
    std::uint64_t done = 0, max_bytes;
    const DownloadProgress *progress;
    bool too_big = false, stopped = false;
};

std::size_t keep_download(char *data, std::size_t size, std::size_t count, void *context) {
    auto *to = static_cast<DownloadTo *>(context);
    std::size_t bytes = size * count;
    if (to->done + bytes > to->max_bytes) {
        to->too_big = true;
        return 0;
    }
    if (std::fwrite(data, 1, bytes, to->file) != bytes)
        return 0;
    to->done += bytes;
    if (*to->progress) {
        std::int64_t total = -1;
        to->curl->easy_getinfo(to->handle, kInfoContentLengthDownloadT, &total);
        if (!(*to->progress)(to->done, total > 0 ? (std::uint64_t)total : 0)) {
            to->stopped = true;
            return 0;
        }
    }
    return bytes;
}

class MacPlatform final : public Platform {
public:
    std::string_view name() const override { return "macos"; }

    fs::path content_dir() const override {
        if (std::string given = env_or_empty("KF4_CONTENT_DIR"); !given.empty())
            return given;
        fs::path dir = program_dir();
        std::error_code error;
        // an .app's Contents/MacOS/<launcher> and Contents/Resources/content
        fs::path resources = dir / ".." / "Resources" / "content";
        if (fs::is_directory(resources, error))
            return resources.lexically_normal();
        return dir / "content";
    }

    // no shipped tools on macOS yet: Xcode's and Homebrew's, whatever KF4_TOOLCHAIN says
    std::optional<Toolchain> toolchain(const fs::path &content, std::string &why) const override {
        (void)content;
        return system_tools(why);
    }

    std::string_view game_program() const override { return "kf4"; }

    std::vector<fs::path> game_files(const fs::path &build_dir) const override {
        std::error_code error;
        if (fs::is_regular_file(build_dir / "kf4", error))
            return {build_dir / "kf4"};
        return {};
    }

    const BuildStrategy &build_strategy() const override { return local_cmake_build(); }

    // (wait's 128 + N: ended by signal N)
    std::string describe_exit(int code) const override {
        std::string text = "code " + std::to_string(code);
        if (code > 128 && code < 128 + NSIG)
            text += std::string(", ended by a signal: ") + strsignal(code - 128);
        else if (code == PORT_EXIT_GPU_LOST)
            text += std::string(", ") + kGpuLostMeaning;
        return text;
    }

    std::unique_ptr<ChildProcess> start(const ProcessSpec &spec, std::string &why) const override {
        int pipe_fds[2] = {-1, -1};
        // (macOS has no pipe2: the ends are made close-on-exec after; the child's dup2 copies are not)
        if (spec.capture_output && (pipe(pipe_fds) != 0 || fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
                                    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) != 0)) {
            why = std::strerror(errno);
            return nullptr;
        }
        // everything the child needs, made before fork (only async-signal-safe calls after it)
        std::vector<std::string> args = spec.argv;
        std::vector<char *> argv;
        for (std::string &arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        std::vector<std::string> env_text;
        for (char **e = environ; *e != nullptr; e++) {
            std::string entry = *e;
            bool replaced = false;
            for (const auto &[name, value] : spec.env)
                replaced |= entry.compare(0, name.size() + 1, name + "=") == 0;
            if (!replaced)
                env_text.push_back(entry);
        }
        for (const auto &[name, value] : spec.env)
            env_text.push_back(name + "=" + value);
        std::vector<char *> envp;
        for (std::string &entry : env_text)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
        std::string dir = spec.working_dir.string();

        pid_t pid = fork();
        if (pid < 0) {
            why = std::strerror(errno);
            if (spec.capture_output) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            return nullptr;
        }
        if (pid == 0) {
            if (spec.stop_with_children)
                setpgid(0, 0);
            if (spec.capture_output) {
                dup2(pipe_fds[1], 1);
                dup2(pipe_fds[1], 2);
            }
            if (!dir.empty() && chdir(dir.c_str()) != 0)
                _exit(127);
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
        if (spec.stop_with_children)
            setpgid(pid, pid); // (both sides, so that a stop right after this finds the group)
        if (spec.capture_output)
            close(pipe_fds[1]);
        return std::make_unique<MacProcess>(pid, spec.capture_output ? pipe_fds[0] : -1);
    }

    std::optional<fs::path> pick_disc() const override {
        // the system's own file chooser, through AppleScript; its answer is the chosen file's POSIX path
        std::vector<std::string> argv = {"/usr/bin/osascript", "-e",
                                         "POSIX path of (choose file with prompt \"Choose your King's Field IV disc "
                                         "(.iso, .chd, or an extracted disc's SYSTEM.CNF)\")"};
        std::string why;
        auto process = start(ProcessSpec{argv, {}, {}, true}, why);
        if (!process)
            return std::nullopt;
        std::string chosen;
        while (auto line = process->read_line())
            if (!line->empty() && (*line)[0] == '/')
                chosen = *line;
        if (process->wait() != 0 || chosen.empty())
            return std::nullopt;
        return fs::path(chosen);
    }

    std::optional<std::string> http_get(std::string_view url, std::string_view user_agent,
                                        int timeout_ms) const override {
        const Curl *curl = load_curl();
        if (curl == nullptr)
            return std::nullopt;
        void *handle = curl->easy_init();
        if (handle == nullptr)
            return std::nullopt;
        std::string url_text(url), agent(user_agent), answer;
        void *headers = curl->slist_append(nullptr, "Accept: application/vnd.github+json");
        curl->easy_setopt(handle, kOptUrl, url_text.c_str());
        curl->easy_setopt(handle, kOptUserAgent, agent.c_str());
        curl->easy_setopt(handle, kOptHttpHeader, headers);
        curl->easy_setopt(handle, kOptFailOnError, 1L);
        curl->easy_setopt(handle, kOptFollowLocation, 1L);
        curl->easy_setopt(handle, kOptNoSignal, 1L);
        curl->easy_setopt(handle, kOptTimeoutMs, (long)timeout_ms);
        curl->easy_setopt(handle, kOptWriteFunction, &keep_answer);
        curl->easy_setopt(handle, kOptWriteData, &answer);
        int result = curl->easy_perform(handle);
        curl->easy_cleanup(handle);
        curl->slist_free_all(headers);
        if (result != 0)
            return std::nullopt;
        return answer;
    }

    std::string http_download(std::string_view url, const fs::path &to, std::uint64_t max_bytes,
                              std::string_view user_agent, const DownloadProgress &progress) const override {
        const Curl *curl = load_curl();
        if (curl == nullptr)
            return "the system's libcurl (/usr/lib/libcurl.4.dylib) could not be loaded";
        std::FILE *file = std::fopen(to.c_str(), "wb");
        if (file == nullptr)
            return "cannot write " + to.string() + ": " + std::strerror(errno);
        void *handle = curl->easy_init();
        if (handle == nullptr) {
            std::fclose(file);
            std::remove(to.c_str());
            return "libcurl did not start";
        }
        std::string url_text(url), agent(user_agent);
        DownloadTo download{curl, handle, file, 0, max_bytes, &progress};
        curl->easy_setopt(handle, kOptUrl, url_text.c_str());
        curl->easy_setopt(handle, kOptUserAgent, agent.c_str());
        curl->easy_setopt(handle, kOptFailOnError, 1L);
        curl->easy_setopt(handle, kOptFollowLocation, 1L);
        curl->easy_setopt(handle, kOptNoSignal, 1L);
        curl->easy_setopt(handle, kOptConnectTimeoutMs, 20000L);
        // a stalled download ends: under 1 KB/s for 30 s
        curl->easy_setopt(handle, kOptLowSpeedLimit, 1024L);
        curl->easy_setopt(handle, kOptLowSpeedTime, 30L);
        curl->easy_setopt(handle, kOptWriteFunction, &keep_download);
        curl->easy_setopt(handle, kOptWriteData, &download);
        int result = curl->easy_perform(handle);
        std::string why;
        if (download.stopped)
            why = "stopped";
        else if (download.too_big)
            why = "the file is larger than expected";
        else if (result != 0)
            why = curl->easy_strerror(result);
        curl->easy_cleanup(handle);
        if (std::fclose(file) != 0 && why.empty())
            why = "cannot write " + to.string() + ": " + std::strerror(errno);
        if (!why.empty())
            std::remove(to.c_str());
        return why;
    }

    std::unique_ptr<Installation> installation(std::string &why) const override {
        why = "the launcher does not update itself on macOS yet (no .app is packaged)";
        return nullptr;
    }

private:
    // Xcode's command line tools (clang) and Homebrew's cmake, ninja and glslang (the renderers' shaders for
    // MoltenVK); SDL3 and MoltenVK from Homebrew too, found through its prefix
    static std::optional<Toolchain> system_tools(std::string &why) {
        Toolchain tools;
        auto cmake = on_path("cmake"), ninja = on_path("ninja"), cc = on_path("clang"), cxx = on_path("clang++");
        auto glslang = on_path("glslangValidator");
        if (!cmake || !ninja || !cc || !cxx || !glslang) {
            why = "building on macOS needs Xcode's command line tools (xcode-select --install) and Homebrew's cmake, "
                  "ninja and glslang (brew install cmake ninja glslang sdl3 molten-vk)";
            return std::nullopt;
        }
        tools.cmake = *cmake;
        tools.ninja = *ninja;
        tools.c_compiler = *cc;
        tools.cxx_compiler = *cxx;
        std::string path = env_or_empty("PATH");
        for (const char *dir : kToolDirs) {
            std::error_code error;
            if (fs::is_directory(fs::path(dir).parent_path(), error)) {
                tools.cmake_options.push_back("-DCMAKE_PREFIX_PATH=" + fs::path(dir).parent_path().string());
                break;
            }
        }
        for (const char *dir : kToolDirs)
            path = std::string(dir) + ":" + path;
        tools.env = {{"PATH", path}};
        tools.id = "macos-system";
        return tools;
    }
};

} // namespace

const Platform &this_platform() {
    static const MacPlatform platform;
    return platform;
}

std::string env_or_empty(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
}

} // namespace launcher
