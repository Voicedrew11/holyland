// The launcher on Linux (platform.h). Processes with fork and exec, each build step in a process group of
// its own so that a cancel stops its compilers too; HTTPS through the system's libcurl, loaded when asked
// for (every desktop distribution has it); the disc chooser through kdialog or zenity.
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "../build_strategy.h"
#include "../platform.h"
#include "../product.h"

extern char **environ;

namespace launcher {

namespace {

fs::path program_dir() {
    std::error_code error;
    fs::path self = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::current_path() : self.parent_path();
}

// a program on PATH
std::optional<fs::path> on_path(std::string_view name) {
    std::string path = env_or_empty("PATH");
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

class LinuxProcess final : public ChildProcess {
public:
    LinuxProcess(pid_t pid, int output) : pid_(pid), output_(output) {}
    ~LinuxProcess() override {
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
constexpr int kOptWriteFunction = 20011;
constexpr std::size_t kMaxAnswer = 1 << 20;

struct Curl {
    void *(*easy_init)();
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    void (*easy_cleanup)(void *);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
};

const Curl *load_curl() {
    static Curl curl;
    static bool loaded = [] {
        void *lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
            lib = dlopen("libcurl-gnutls.so.4", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
            return false;
        curl.easy_init = reinterpret_cast<void *(*)()>(dlsym(lib, "curl_easy_init"));
        curl.easy_setopt = reinterpret_cast<int (*)(void *, int, ...)>(dlsym(lib, "curl_easy_setopt"));
        curl.easy_perform = reinterpret_cast<int (*)(void *)>(dlsym(lib, "curl_easy_perform"));
        curl.easy_cleanup = reinterpret_cast<void (*)(void *)>(dlsym(lib, "curl_easy_cleanup"));
        curl.slist_append = reinterpret_cast<void *(*)(void *, const char *)>(dlsym(lib, "curl_slist_append"));
        curl.slist_free_all = reinterpret_cast<void (*)(void *)>(dlsym(lib, "curl_slist_free_all"));
        return curl.easy_init && curl.easy_setopt && curl.easy_perform && curl.easy_cleanup && curl.slist_append &&
               curl.slist_free_all;
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

class LinuxPlatform final : public Platform {
public:
    std::string_view name() const override { return "linux"; }

    fs::path content_dir() const override {
        if (std::string given = env_or_empty("KF4_CONTENT_DIR"); !given.empty())
            return given;
        fs::path dir = program_dir();
        std::error_code error;
        // the AppImage's usr/bin/<id> and usr/share/<id>/content
        fs::path shared = dir / ".." / "share" / kProductId / "content";
        if (fs::is_directory(shared, error))
            return shared.lexically_normal();
        return dir / "content";
    }

    std::optional<Toolchain> toolchain(const fs::path &content, std::string &why) const override {
        if (env_or_empty("KF4_TOOLCHAIN") == "system")
            return system_tools(why);
        fs::path root = content / "toolchain";
        Toolchain tools;
        tools.cmake = root / "cmake/bin/cmake";
        tools.ninja = root / "bin/ninja";
        tools.c_compiler = root / "llvm/bin/clang";
        tools.cxx_compiler = root / "llvm/bin/clang++";
        for (const fs::path &tool : {tools.cmake, tools.ninja, tools.c_compiler, tools.cxx_compiler}) {
            if (access(tool.c_str(), X_OK) != 0) {
                why = tool.string() + " is missing";
                return std::nullopt;
            }
        }
        tools.cmake_options = {"-DCMAKE_TOOLCHAIN_FILE=" + (root / "linux.cmake").generic_string()};
        tools.runtime_files = {root / "lib/libSDL3.so.0"};
        tools.env = {{"PATH", (root / "llvm/bin").string() + ":" + env_or_empty("PATH")}};
        tools.id = "linux:" + read_id(root / "ID");
        return tools;
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
        return text;
    }

    std::unique_ptr<ChildProcess> start(const ProcessSpec &spec, std::string &why) const override {
        int pipe_fds[2] = {-1, -1};
        if (spec.capture_output && pipe2(pipe_fds, O_CLOEXEC) != 0) {
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
        return std::make_unique<LinuxProcess>(pid, spec.capture_output ? pipe_fds[0] : -1);
    }

    std::optional<fs::path> pick_disc() const override {
        std::string home = env_or_empty("HOME");
        std::vector<std::string> argv;
        bool kde = env_or_empty("XDG_CURRENT_DESKTOP").find("KDE") != std::string::npos;
        auto kdialog = on_path("kdialog");
        auto zenity = on_path("zenity");
        if (kdialog && (kde || !zenity))
            argv = {kdialog->string(), "--title", "Choose your King's Field IV disc", "--getopenfilename", home,
                    "*.iso *.ISO *.chd *.CHD SYSTEM.CNF|King's Field IV disc (.iso, .chd, or an extracted disc's SYSTEM.CNF)"};
        else if (zenity)
            argv = {zenity->string(), "--file-selection", "--title=Choose your King's Field IV disc",
                    "--file-filter=King's Field IV disc | *.iso *.ISO *.chd *.CHD SYSTEM.CNF", "--file-filter=All files | *"};
        else
            return std::nullopt;
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

private:
    static std::string read_id(const fs::path &file) {
        FILE *f = std::fopen(file.c_str(), "rb");
        if (f == nullptr)
            return "unknown";
        char buffer[256];
        std::size_t got = std::fread(buffer, 1, sizeof(buffer), f);
        std::fclose(f);
        std::string id(buffer, got);
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r'))
            id.pop_back();
        return id;
    }

    // a developer's run: the tools on PATH (KF4_TOOLCHAIN=system)
    static std::optional<Toolchain> system_tools(std::string &why) {
        Toolchain tools;
        auto cmake = on_path("cmake"), ninja = on_path("ninja"), cc = on_path("clang"), cxx = on_path("clang++");
        if (!cmake || !ninja || !cc || !cxx) {
            why = "KF4_TOOLCHAIN=system needs cmake, ninja, clang and clang++ on PATH";
            return std::nullopt;
        }
        tools.cmake = *cmake;
        tools.ninja = *ninja;
        tools.c_compiler = *cc;
        tools.cxx_compiler = *cxx;
        tools.id = "system";
        return tools;
    }
};

} // namespace

const Platform &this_platform() {
    static const LinuxPlatform platform;
    return platform;
}

std::string env_or_empty(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
}

} // namespace launcher
