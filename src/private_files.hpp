#pragma once

// Files sink writes for a shell to run: the stub commands on PATH, and the
// shell integration scripts. Anything a shell executes has to be somewhere
// nobody but the user can write, or whoever can write there runs code as the
// user in every shell sink opens. Everything here checks that, and gives up --
// returning false or an empty path -- rather than use a location that fails.

#include <string>
#include <sys/types.h>

namespace private_files {

// ~/.config/sink, created if missing. ~/.config and ~/.config/sink may be
// symlinks (dotfile managers make them so); what they lead to must be a
// directory owned by this user that nobody else can write. Empty if not.
std::string config_root();

// A directory sink owns inside config_root(): created 0700 if missing, and
// accepted only if it is a real directory -- not a symlink -- owned by this
// user and writable by nobody else.
bool ensure_dir(const std::string& path);

// Writes `content` to `path` with permissions `mode`, replacing whatever was
// there. Unlinked first and opened O_EXCL | O_NOFOLLOW, so a symlink planted at
// the path can't redirect the write somewhere else.
bool write_file(const std::string& path, const std::string& content, mode_t mode);

} // namespace private_files
