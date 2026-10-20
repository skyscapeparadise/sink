#include "shell_integration.hpp"
#include "private_files.hpp"

#include <cstdlib>

namespace shell_integration {
namespace {

struct ShellScript {
    const char* path;      // relative to the install directory
    const char* content;
};
#include "shell_integration_scripts.inc"

// What sink adds to bash's PROMPT_COMMAND. sink.bash matches this exact text
// to take it back out, so the two must agree.
constexpr const char* kBashLoader = "builtin source \"$SINK_BASH_INTEGRATION\"";

std::string basename_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::string install(const std::string& config_root) {
    if (config_root.empty()) return {};
    const std::string dir = config_root + "/shell-integration";
    if (!private_files::ensure_dir(dir) ||
        !private_files::ensure_dir(dir + "/zsh") ||
        !private_files::ensure_dir(dir + "/bash")) {
        return {};
    }
    // Rewritten every launch, so the scripts always match this build. 0600:
    // they are sourced, never executed.
    for (const ShellScript& script : kShellScripts) {
        if (!private_files::write_file(dir + "/" + script.path, script.content, 0600)) return {};
    }
    return dir;
}

Launch plan(const std::string& shell, const std::string& dir, bool login,
            const std::vector<std::string>& args,
            const std::vector<std::pair<std::string, std::string>>& env) {
    Launch launch;
    launch.argv = { shell };
    if (login) launch.argv.push_back("-l");
    launch.argv.insert(launch.argv.end(), args.begin(), args.end());
    if (dir.empty()) return launch;

    // [shell.env] first, then what sink itself was started with.
    auto lookup = [&](const char* name) -> const char* {
        for (const auto& kv : env) {
            if (kv.first == name) return kv.second.c_str();
        }
        return getenv(name);
    };

    const std::string name = basename_of(shell);
    if (name == "zsh") {
        // zsh reads $ZDOTDIR/.zshenv first; pointing ZDOTDIR at the
        // integration makes that its bootstrap, which puts ZDOTDIR back
        // before anything of yours runs. What to put back travels in
        // SINK_ZSH_ZDOTDIR -- absent meaning ZDOTDIR wasn't set at all.
        launch.set.push_back({ "ZDOTDIR", dir + "/zsh" });
        launch.set.push_back({ "SINK_SHELL_INTEGRATION_DIR", dir });
        if (const char* orig = lookup("ZDOTDIR")) {
            launch.set.push_back({ "SINK_ZSH_ZDOTDIR", orig });
        } else {
            launch.unset.push_back("SINK_ZSH_ZDOTDIR");
        }
    } else if (name == "bash") {
        // bash starts as a plain login shell, its startup untouched. The
        // integration loads from PROMPT_COMMAND, just before the first
        // prompt, and takes its loader back out (see sink.bash). Starting it
        // in POSIX mode with ENV, as some terminals do, needs sink to replay
        // bash's startup order itself -- and bash 3.2, macOS's own, ignores
        // ENV in a login shell, leaving POSIX mode on and the integration
        // unloaded.
        launch.set.push_back({ "SINK_BASH_INTEGRATION", dir + "/bash/sink.bash" });
        std::string prompt_command = kBashLoader;
        if (const char* orig = lookup("PROMPT_COMMAND"); orig && *orig) {
            prompt_command = std::string(orig) + "\n" + kBashLoader;
        }
        launch.set.push_back({ "PROMPT_COMMAND", prompt_command });
    }
    return launch;
}

} // namespace shell_integration
