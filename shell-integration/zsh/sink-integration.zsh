# sink shell integration for zsh.
#
# Loaded by the .zshenv beside this file, which sink has zsh read first. It
# tells sink what the shell knows and the screen doesn't say, in escape
# sequences only sink acts on -- other terminals ignore them:
#
#   OSC 133;A      a prompt starts here: prompt jumps (Cmd+Up/Down)
#   OSC 133;B      the command line starts here: mouse editing
#   OSC 133;C      the command is running: that line is finished
#   OSC 7          the current directory: new tabs and splits start there
#   OSC 6973;L=n   the command line is n characters long, so a click past its
#                  end can't step into text that isn't part of it -- an
#                  autosuggestion, which a step to the right would accept
#
# Nothing here changes how the shell behaves. To turn it off, put
#   shell_integration = false
# at the top of ~/.config/sink/sink.toml.

# Once per shell.
[[ -n ${_sink_integration-} ]] && return
typeset -g _sink_integration=1

builtin autoload -Uz add-zsh-hook

# The hooks go in at the first prompt rather than now. This file runs before
# your .zshrc, and that is where themes and frameworks install their hooks;
# going in after them is what lets the prompt marks survive a theme.
_sink_first_prompt() {
    local ret=$?
    add-zsh-hook -d precmd _sink_first_prompt
    add-zsh-hook precmd _sink_precmd
    add-zsh-hook preexec _sink_preexec
    builtin autoload -Uz is-at-least
    if is-at-least 5.3; then
        builtin autoload -Uz add-zle-hook-widget
        add-zle-hook-widget line-init _sink_line_init
        add-zle-hook-widget line-pre-redraw _sink_line_pre_redraw
    fi
    _sink_precmd
    return ret
}
add-zsh-hook precmd _sink_first_prompt

_sink_precmd() {
    local ret=$?
    local percent=0
    [[ -o prompt_percent ]] && percent=1
    builtin emulate -L zsh

    # Last in line from here on, so a theme that rebuilds PS1 in its own
    # hook has done it by the time the marks go on.
    if [[ ${precmd_functions[-1]-} != _sink_precmd ]]; then
        precmd_functions=(${precmd_functions:#_sink_precmd} _sink_precmd)
    fi

    _sink_report_cwd

    # A at the start of the prompt, B at its end -- the end being where the
    # command line begins. Inside %{ %} so zsh knows they take no columns.
    # Stripped first, so a PS1 that is not rebuilt doesn't collect copies.
    if (( percent )); then
        local a=$'%{\e]133;A\a%}' b=$'%{\e]133;B\a%}'
        PS1=${PS1//${(b)a}/}
        PS1=${PS1//${(b)b}/}
        PS1=$a$PS1$b
    fi
    return ret
}

_sink_preexec() {
    builtin print -rn -- $'\e]133;C\a'
}

# The command line has begun, with the prompt drawn. B is already in PS1
# whenever _sink_precmd ran last; at the very first prompt a theme can rebuild
# PS1 after it, so the marks are sent from here too -- A only if PS1 has lost
# it, since A belongs at the prompt's first row and this is its last.
_sink_line_init() {
    if [[ $PS1 != *$'\e]133;A\a'* ]]; then
        builtin print -rn -- $'\e]133;A\a'
    fi
    builtin print -rn -- $'\e]133;B\a'
}

_sink_line_pre_redraw() {
    builtin print -rn -- $'\e]6973;L='"${#BUFFER}"$'\a'
}

# file://host/path, with the path percent-encoded byte by byte.
_sink_report_cwd() {
    builtin emulate -L zsh
    builtin setopt extended_glob no_multibyte
    local encoded=${PWD//(#m)[^A-Za-z0-9\/._~-]/%${(l:2::0:)$(( [##16] #MATCH ))}}
    builtin print -rn -- $'\e]7;file://'"${HOST}${encoded}"$'\a'
}
