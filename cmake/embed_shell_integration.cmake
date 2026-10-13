# Embeds the shell integration scripts in the binary (see
# src/shell_integration.cpp), so the app always installs exactly the version
# it was built with. Run at build time:
#   cmake -DSRC=<shell-integration dir> -DOUT=<header> -P embed_shell_integration.cmake
set(FILES zsh/.zshenv zsh/sink-integration.zsh bash/sink.bash)
set(DELIM "SINK_SCRIPT")  # raw-string delimiters may be at most 16 characters
set(BODY "// Generated from shell-integration/ by cmake/embed_shell_integration.cmake.\n// Edit the scripts, not this file.\n")
string(APPEND BODY "static const ShellScript kShellScripts[] = {\n")
foreach(f ${FILES})
    file(READ "${SRC}/${f}" content)
    string(FIND "${content}" ")${DELIM}\"" clash)
    if(NOT clash EQUAL -1)
        message(FATAL_ERROR "${f} contains the raw-string delimiter )${DELIM}\"")
    endif()
    string(APPEND BODY "    { \"${f}\", R\"${DELIM}(${content})${DELIM}\" },\n")
endforeach()
string(APPEND BODY "};\n")
file(WRITE "${OUT}.tmp" "${BODY}")
# Only touch the header when it changes, so unchanged scripts don't rebuild.
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
