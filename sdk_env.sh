#!/bin/bash
# sdk_env.sh - Chooses the macOS SDK and deployment target, and exports both.
#
# Sourced (not executed) by build.sh and releases/package.sh, which both
# compile and so both need the same answer. Expects to be sourced from the
# repo root.
#
# Why this needs choosing at all:
#
# The SDK and the linker have to come from the same toolchain. An OS update
# drops a newer SDK into /Library/Developer/CommandLineTools and repoints
# .../SDKs/MacOSX.sdk at it, and that is what xcrun then reports as the default
# sysroot -- but the linker from the *installed Xcode* can be older than that
# SDK and unable to parse it. macOS 27.0's .tbd files declare an arm64e.x1
# slice that ld-1267 rejects as "unknown architecture", so every link fails
# with a wall of tapi errors that say nothing about the real cause.
#
# So prefer the SDK that ships inside the selected Xcode. That one is a matched
# pair with the linker being used, by construction, and it moves forward on its
# own the next time Xcode is updated -- nothing here needs editing again.

# A sysroot is only usable if it links, not just if it exists: the failure mode
# this guards against is a present, readable, *unparseable* SDK.
_sink_sdk_links() {
    local probe ok=1
    probe="$(mktemp -d)"
    printf 'int main(void){return 0;}\n' > "${probe}/t.c"
    if /usr/bin/cc -isysroot "$1" "${probe}/t.c" -o "${probe}/t" >/dev/null 2>&1; then
        ok=0
    fi
    rm -rf "${probe}"
    return ${ok}
}

_sink_developer_dir="$(xcode-select -p 2>/dev/null || true)"
_sink_sdk_candidates=()
if [ -n "${_sink_developer_dir}" ]; then
    _sink_sdk_candidates+=("${_sink_developer_dir}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk")
fi
# The default, second: it is right on a machine whose toolchain is consistent,
# and wrong in exactly the case above.
_sink_sdk_candidates+=("$(xcrun --show-sdk-path 2>/dev/null || true)")

SDKROOT=""
for _sink_candidate in "${_sink_sdk_candidates[@]}"; do
    if [ -n "${_sink_candidate}" ] && [ -d "${_sink_candidate}" ] \
       && _sink_sdk_links "${_sink_candidate}"; then
        SDKROOT="${_sink_candidate}"
        break
    fi
done

if [ -z "${SDKROOT}" ]; then
    echo "❌ No macOS SDK found that the installed linker can link against." >&2
    echo "   Tried:" >&2
    for _sink_candidate in "${_sink_sdk_candidates[@]}"; do
        [ -n "${_sink_candidate}" ] && echo "     ${_sink_candidate}" >&2
    done
    echo "   Installing or updating Xcode, then" >&2
    echo "   'sudo xcode-select -s /Applications/Xcode.app', should give a" >&2
    echo "   linker and an SDK from the same toolchain." >&2
    exit 1
fi

# Exported, not just passed to CMake: the vendored dependencies are built by
# configure/make rather than CMake, and clang honours SDKROOT from the
# environment. Without this they would pick the broken default on their next
# rebuild and fail the same way, several minutes into an ffmpeg build.
export SDKROOT

# Read out of CMakeLists.txt rather than repeated here, the same way
# package.sh reads the version: CMakeLists.txt is where the minimum macOS
# version is decided, and a second copy is a second thing to forget.
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(sed -nE \
    's/.*set\(CMAKE_OSX_DEPLOYMENT_TARGET "([0-9.]+)".*/\1/p' CMakeLists.txt | head -1)}"
if [ -z "${MACOSX_DEPLOYMENT_TARGET}" ]; then
    echo "❌ Could not read CMAKE_OSX_DEPLOYMENT_TARGET out of CMakeLists.txt." >&2
    exit 1
fi
export MACOSX_DEPLOYMENT_TARGET

echo "  using SDK: ${SDKROOT}"
echo "  targeting macOS ${MACOSX_DEPLOYMENT_TARGET}+"
