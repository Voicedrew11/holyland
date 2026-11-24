// The launcher's files in the data folder (launcher_files.h)
#include "launcher_files.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include "build_key.h"
#include "cJSON.h"

extern "C" {
#include "port/data_dir.h"
}

namespace launcher {

namespace {

const char *read_env(const char *name) {
    return std::getenv(name);
}

const char *string_item(const cJSON *root, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

void set_item(cJSON *root, const char *name, cJSON *value) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, name);
    cJSON_AddItemToObject(root, name, value);
}

struct JsonDeleter {
    void operator()(cJSON *json) const { cJSON_Delete(json); }
};

} // namespace

std::optional<DataPaths> data_paths() {
    char data[1024], builds[1024];
    DataDirSystem system = data_dir_this_system();
    data_dir_move_old_folders(system, read_env); // the folders' old name, darker-reality (does nothing once moved)
    if (!data_dir_resolve(system, read_env, data, sizeof(data)) ||
        !data_dir_resolve_builds(system, read_env, builds, sizeof(builds)))
        return std::nullopt;
    return DataPaths{fs::u8path(data), fs::u8path(builds)};
}

std::FILE *open_file(const fs::path &file, const char *mode) {
#ifdef _WIN32
    std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfopen(file.wstring().c_str(), wide_mode.c_str());
#else
    return std::fopen(file.c_str(), mode);
#endif
}

std::optional<std::string> read_file(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string file_sha256(const fs::path &file) {
    std::FILE *f = open_file(file, "rb");
    if (f == nullptr)
        return {};
    Sha256Hasher hasher;
    std::string buffer(1 << 20, '\0');
    std::size_t got;
    while ((got = std::fread(buffer.data(), 1, buffer.size(), f)) > 0)
        hasher.add(std::string_view(buffer.data(), got));
    bool failed = std::ferror(f) != 0;
    std::fclose(f);
    return failed ? std::string() : to_hex(hasher.finish());
}

#ifdef _WIN32
bool move_replacing(const fs::path &from, const fs::path &to) {
    DWORD waited_ms = 0, pause_ms = 10;
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        DWORD error = GetLastError();
        bool held_open = error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
        if (!held_open || waited_ms >= 1000)
            return false;
        Sleep(pause_ms);
        waited_ms += pause_ms;
        pause_ms = pause_ms < 200 ? pause_ms * 2 : pause_ms;
    }
}
#else
bool move_replacing(const fs::path &from, const fs::path &to) {
    return std::rename(from.c_str(), to.c_str()) == 0;
}
#endif

bool write_file_atomically(const fs::path &file, const std::string &text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    fs::path temp = file;
    temp += ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        out.flush();
        if (!out)
            return false;
    }
    bool moved = move_replacing(temp, file);
    if (!moved)
        fs::remove(temp, error);
    return moved;
}

LauncherChoices read_choices(const fs::path &file) {
    LauncherChoices choices;
    auto text = read_file(file);
    if (!text)
        return choices;
    std::unique_ptr<cJSON, JsonDeleter> root(cJSON_Parse(text->c_str()));
    if (!cJSON_IsObject(root.get()))
        return choices;
    if (const char *disc = string_item(root.get(), "disc"))
        choices.disc = disc;
    if (const char *skipped = string_item(root.get(), "skipped"))
        choices.skipped = skipped;
    const cJSON *check = cJSON_GetObjectItemCaseSensitive(root.get(), "update_check");
    if (cJSON_IsBool(check))
        choices.update_check = cJSON_IsTrue(check);
    const cJSON *setup = cJSON_GetObjectItemCaseSensitive(root.get(), "setup_done");
    if (cJSON_IsBool(setup))
        choices.setup_done = cJSON_IsTrue(setup);
    return choices;
}

bool write_choices(const fs::path &file, const LauncherChoices &choices) {
    auto old = read_file(file);
    std::unique_ptr<cJSON, JsonDeleter> root(old ? cJSON_Parse(old->c_str()) : nullptr);
    if (!cJSON_IsObject(root.get()))
        root.reset(cJSON_CreateObject());
    if (!root)
        return false;
    set_item(root.get(), "disc", cJSON_CreateString(choices.disc.c_str()));
    set_item(root.get(), "update_check", cJSON_CreateBool(choices.update_check));
    set_item(root.get(), "skipped", cJSON_CreateString(choices.skipped.c_str()));
    set_item(root.get(), "setup_done", cJSON_CreateBool(choices.setup_done));
    char *text = cJSON_Print(root.get());
    if (text == nullptr)
        return false;
    std::string out = std::string(text) + "\n";
    cJSON_free(text);
    return write_file_atomically(file, out);
}

std::string remove_tree(const fs::path &dir) {
    std::error_code error;
    if (!fs::exists(fs::symlink_status(dir, error)))
        return {};
    // writable first, folders as well as files (fs::permissions on Windows clears the read-only attribute)
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, error);
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        std::error_code ignored;
        if (it->is_symlink(ignored))
            continue;
        fs::perms wanted = it->is_directory(ignored) ? fs::perms::owner_all : fs::perms::owner_write;
        fs::permissions(it->path(), wanted, fs::perm_options::add, ignored);
    }
    fs::remove_all(dir, error);
    if (error)
        return "cannot remove " + dir.u8string() + ": " + error.message();
    if (fs::exists(fs::symlink_status(dir, error)))
        return "cannot remove " + dir.u8string();
    return {};
}

} // namespace launcher
