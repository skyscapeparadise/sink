#include "private_files.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace private_files {
namespace {

bool private_to_user(const struct stat& st) {
    return S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

// Creates `path` if it doesn't exist, then checks what is there, following a
// symlink to its target.
bool ensure_followed(const std::string& path, mode_t mode) {
    if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) return false;
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && private_to_user(st);
}

} // namespace

std::string config_root() {
    const char* home = getenv("HOME");
    if (!home || !*home) return {};
    std::string config = std::string(home) + "/.config";
    if (!ensure_followed(config, 0755)) return {};
    std::string root = config + "/sink";
    if (!ensure_followed(root, 0700)) return {};
    return root;
}

bool ensure_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    // Vet what is there rather than assume mkdir made it: EEXIST is tolerated
    // so a later launch reuses the first's work, which means the path may be
    // anything at all -- a file, a symlink, someone else's directory.
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode) && private_to_user(st);
}

bool write_file(const std::string& path, const std::string& content, mode_t mode) {
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    bool ok = ::fchmod(fd, mode) == 0;
    return ::close(fd) == 0 && ok;
}

} // namespace private_files
