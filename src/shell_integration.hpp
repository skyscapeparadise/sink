#pragma once

// Built-in shell integration: sink loads a small script into the zsh or bash
// it starts, which reports where each prompt and command line begins
// (OSC 133), the current directory (OSC 7) and, in zsh, the command line's
// length. The scripts are in shell-integration/ and say what they do.
//
// The rule they are held to: your own startup files load exactly as they
// would without sink, from the same places, in the same order. zsh reads a
// bootstrap first (via ZDOTDIR) that puts ZDOTDIR back before any of yours;
// bash isn't touched at startup at all, and loads the integration from
// PROMPT_COMMAND at its first prompt. Turned off with
// `shell_integration = false` in sink.toml.

#include <string>
#include <utility>
#include <vector>

namespace shell_integration {

// Writes the scripts into `<config_root>/shell-integration`, a directory only
// this user can write -- a shell runs them, so anyone who could edit them
// could run code in every shell sink starts. Returns that directory, or an
// empty string if it couldn't be done safely (then shells start plainly).
std::string install(const std::string& config_root);

// How to start `shell` as a login shell: its argv, and the environment to set
// and unset in the child. With `dir` empty, or a shell other than zsh or bash,
// that is just `shell -l`. Reads the current environment, so call it in the
// parent before forking.
struct Launch {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::string> unset;
};
Launch plan(const std::string& shell, const std::string& dir);

} // namespace shell_integration
