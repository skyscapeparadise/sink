# sink shell integration for zsh: bootstrap. See sink-integration.zsh.
#
# sink starts zsh with ZDOTDIR pointing at this folder, so that zsh reads
# this file in place of your .zshenv. Its first job is to undo that: ZDOTDIR
# goes back to what it was -- or away, if it was never set -- so the rest of
# zsh's startup finds your .zshenv, .zprofile, .zshrc and .zlogin exactly
# where it always does, in the order it always does.
if [[ -n ${SINK_ZSH_ZDOTDIR+set} ]]; then
    builtin export ZDOTDIR=$SINK_ZSH_ZDOTDIR
else
    builtin unset ZDOTDIR
fi
builtin unset SINK_ZSH_ZDOTDIR

# zsh read this file instead of your .zshenv, so read yours now.
if [[ -f ${ZDOTDIR:-$HOME}/.zshenv ]]; then
    builtin source ${ZDOTDIR:-$HOME}/.zshenv
fi

# The integration, for interactive shells only. Its own variable goes once
# used, so shells started from this one are left alone.
if [[ -o interactive && -n ${SINK_SHELL_INTEGRATION_DIR-} ]]; then
    builtin source $SINK_SHELL_INTEGRATION_DIR/zsh/sink-integration.zsh
fi
builtin unset SINK_SHELL_INTEGRATION_DIR
