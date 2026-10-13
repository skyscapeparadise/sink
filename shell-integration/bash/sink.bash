# sink shell integration for bash.
#
# Tells sink what the shell knows and the screen doesn't say, in escape
# sequences only sink acts on -- other terminals ignore them:
#
#   OSC 133;A   a prompt starts here: prompt jumps (Cmd+Up/Down)
#   OSC 133;B   the command line starts here: mouse editing
#   OSC 133;C   the command is running: that line is finished (bash 4.4+)
#   OSC 7       the current directory: new tabs and splits start there
#
# Nothing here changes how the shell behaves. To turn it off, put
#   shell_integration = false
# at the top of ~/.config/sink/sink.toml.
#
# How it gets loaded: it doesn't touch startup at all. bash starts exactly as
# it would anywhere else -- a normal login shell reading your startup files in
# its normal order. sink only puts a loader in PROMPT_COMMAND in bash's
# environment, so this file is read just before the first prompt, after all
# of those files have run. If they replace PROMPT_COMMAND outright, it never
# loads, and the shell simply runs without integration.

_sink_status=$?
if [[ -n ${_sink_integration-} || $- != *i* ]]; then return $_sink_status; fi
_sink_integration=1

# The loader has done its job. Swap it for ':' so the commands around it still
# parse, and stop handing PROMPT_COMMAND and the loader's path down to other
# shells started from this one.
_sink_loader='builtin source "$SINK_BASH_INTEGRATION"'
if [[ "$(builtin declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
    for _sink_i in "${!PROMPT_COMMAND[@]}"; do
        PROMPT_COMMAND[_sink_i]=${PROMPT_COMMAND[_sink_i]//"$_sink_loader"/:}
    done
    PROMPT_COMMAND+=(_sink_prompt_command)
else
    PROMPT_COMMAND=${PROMPT_COMMAND//"$_sink_loader"/:}
    PROMPT_COMMAND="$PROMPT_COMMAND"$'\n'_sink_prompt_command
fi
builtin export -n PROMPT_COMMAND
builtin unset SINK_BASH_INTEGRATION _sink_loader _sink_i

# A at the start of the prompt, B at its end -- where the command line
# begins. Inside \[ \] so readline knows they take no columns.
_sink_mark_a='\[\e]133;A\a\]'
_sink_mark_b='\[\e]133;B\a\]'

# file://host/path, with the path percent-encoded byte by byte.
_sink_report_cwd() {
    local LC_ALL=C path="$PWD" encoded='' c i
    for (( i = 0; i < ${#path}; i++ )); do
        c=${path:i:1}
        case $c in
            [A-Za-z0-9/._~-]) encoded+=$c ;;
            *) builtin printf -v c '%%%02X' "'$c"; encoded+=$c ;;
        esac
    done
    builtin printf '\e]7;file://%s%s\a' "$HOSTNAME" "$encoded"
}

# Last among the prompt commands from the second prompt on (it was appended
# after everything your startup files put there), so the marks go onto PS1
# after anything that rebuilds it.
_sink_prompt_command() {
    local ret=$?
    _sink_report_cwd
    PS1=${PS1//"$_sink_mark_a"/}
    PS1=${PS1//"$_sink_mark_b"/}
    PS1="$_sink_mark_a$PS1$_sink_mark_b"
    return $ret
}

# PS0 is printed once a command has been read, just before it runs.
if (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
    PS0="${PS0-}"'\e]133;C\a'
fi

# This first prompt: the appended command only runs from the next one.
_sink_prompt_command
return $_sink_status
