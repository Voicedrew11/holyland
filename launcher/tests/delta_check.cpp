// A release's deltas checked with the launcher's own code (update_delta.h): each must make its whole file exactly
// from the older release's. packaging/check_deltas.sh runs it for every delta of a release, in CI and before a
// release is made.
//
//   launcher_delta_check file OLD_FILE DELTA NEW_FILE
//   launcher_delta_check tree OLD_TREE DELTA_TREE NEW_TREE   (the zips unpacked; DELTA_TREE is left as it was)
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <system_error>

#include "launcher_files.h"
#include "update_delta.h"

using namespace launcher;

namespace {

// every file under `root` and its SHA-256, by its path inside
std::map<std::string, std::string> tree_files(const fs::path &root) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error))
            files[it->path().lexically_relative(root).generic_u8string()] = file_sha256(it->path());
    }
    return files;
}

int check_file(const fs::path &old_file, const fs::path &delta, const fs::path &new_file) {
    fs::path made = fs::temp_directory_path() / "holyland_delta_check.made";
    std::string whole = file_sha256(new_file);
    std::string why = apply_file_patch(old_file, delta, made, whole, kUpdateMaxBytes);
    std::error_code error;
    fs::remove(made, error);
    if (!why.empty()) {
        std::fprintf(stderr, "FAIL %s: %s\n", delta.u8string().c_str(), why.c_str());
        return 1;
    }
    std::printf("ok   %s makes %s\n", delta.filename().u8string().c_str(), new_file.filename().u8string().c_str());
    return 0;
}

int check_tree(const fs::path &old_tree, const fs::path &delta_tree, const fs::path &new_tree) {
    fs::path staged = fs::temp_directory_path() / "holyland_delta_check.tree";
    std::error_code error;
    fs::remove_all(staged, error);
    fs::copy(delta_tree, staged, fs::copy_options::recursive, error);
    if (error) {
        std::fprintf(stderr, "FAIL cannot copy %s: %s\n", delta_tree.u8string().c_str(), error.message().c_str());
        return 1;
    }
    std::string why = complete_tree(staged, old_tree);
    bool same = why.empty() && tree_files(staged) == tree_files(new_tree);
    fs::remove_all(staged, error);
    if (!same) {
        std::fprintf(stderr, "FAIL %s: %s\n", delta_tree.u8string().c_str(),
                     why.empty() ? "what it makes is not the new release's tree" : why.c_str());
        return 1;
    }
    std::printf("ok   %s makes %s\n", delta_tree.filename().u8string().c_str(),
                new_tree.filename().u8string().c_str());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "file") == 0)
        return check_file(fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4]));
    if (argc == 5 && std::strcmp(argv[1], "tree") == 0)
        return check_tree(fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4]));
    std::fprintf(stderr, "usage: %s file OLD_FILE DELTA NEW_FILE | tree OLD_TREE DELTA_TREE NEW_TREE\n", argv[0]);
    return 2;
}
