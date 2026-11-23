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
#ifdef _WIN32
    bool moved = MoveFileExW(temp.wstring().c_str(), file.wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    bool moved = std::rename(temp.string().c_str(), file.string().c_str()) == 0;
#endif
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

} // namespace launcher
