#!/bin/bash
# package.sh - Packages the sink terminal into a standalone macOS App Bundle (sink.app)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

echo "packaging sink.app..."

# 1. Compile the latest binary
echo "  compiling project..."
cmake -B build -S .
cmake --build build

# 2. Setup the App Bundle folders
APP_NAME="sink"
APP_DIR="${APP_NAME}.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"

echo "  creating bundle directory tree..."
rm -rf "${APP_DIR}"
mkdir -p "${MACOS_DIR}"
mkdir -p "${RESOURCES_DIR}"

# 3. Copy the compiled executable and assets
echo "  copying binary..."
cp build/sink "${MACOS_DIR}/${APP_NAME}"

echo "  copying app icon..."
if [ -f "icon.icns" ]; then
    cp icon.icns "${RESOURCES_DIR}/icon.icns"
else
    echo "  warning: icon.icns not found! app bundle will use default fallback icon."
fi

echo "  copying background video..."
if [ -f "sinkpool.mp4" ]; then
    cp sinkpool.mp4 "${RESOURCES_DIR}/sinkpool.mp4"
fi

echo "  copying default monaspace neon font..."
if [ -f "fonts/MonaspaceNeon-Regular.otf" ]; then
    cp fonts/MonaspaceNeon-Regular.otf "${RESOURCES_DIR}/MonaspaceNeon-Regular.otf"
fi

echo "  copying mona sans font (settings UI)..."
if [ -f "fonts/MonaSans-VariableFont.ttf" ]; then
    cp "fonts/MonaSans-VariableFont.ttf" "${RESOURCES_DIR}/MonaSans-VariableFont.ttf"
fi

echo "  copying logo assets..."
if [ -f "logos/sinklogo.svg" ]; then
    cp logos/sinklogo.svg "${RESOURCES_DIR}/sinklogo.svg"
fi
if [ -f "logos/rainlogo.svg" ]; then
    cp logos/rainlogo.svg "${RESOURCES_DIR}/rainlogo.svg"
fi

echo "  copying chunked binary demo archive..."
mkdir -p "${RESOURCES_DIR}/demo"
if [ -f "demo/splash.dat" ]; then
    cp demo/splash.dat "${RESOURCES_DIR}/demo/splash.dat"
fi

# Every dylib in Contents/Frameworks carries an attribution requirement of some
# kind -- FreeType's FTL and libpng's license both require the notice be
# reproduced, and FFmpeg's LGPL requires conveying the license plus a route to
# the source. Ship them all rather than tracking which ones are strict about it.
echo "  copying third-party license texts..."
LICENSE_DIR="${RESOURCES_DIR}/licenses"
mkdir -p "${LICENSE_DIR}"
copy_license() {
    src="$1"; dst="$2"
    if [ -f "${src}" ]; then
        cp "${src}" "${LICENSE_DIR}/${dst}"
    else
        echo "  warning: license text not found: ${src}"
    fi
}
copy_license "vendor/ffmpeg/COPYING.LGPLv2.1"   "FFmpeg-COPYING.LGPLv2.1"
copy_license "vendor/ffmpeg/README.sink"        "FFmpeg-README.txt"
copy_license "vendor/textstack/COPYING"         "HarfBuzz-COPYING"
copy_license "vendor/textstack/LICENSE.txt"     "SDL3_ttf-LICENSE.txt"
copy_license "vendor/sdl3_image/LICENSE.txt"    "SDL3_image-LICENSE.txt"
copy_license "/opt/homebrew/opt/sdl3/LICENSE.txt"     "SDL3-LICENSE.txt"
copy_license "/opt/homebrew/opt/freetype/LICENSE.TXT" "FreeType-LICENSE.txt"
copy_license "/opt/homebrew/opt/libpng/LICENSE"       "libpng-LICENSE"

# 4. Bundle third-party dylibs into the app
#
# The binary links against Homebrew's SDL3/SDL3_ttf/SDL3_image/ffmpeg trees by
# absolute path (/opt/homebrew/opt/...). Those paths only exist on a machine
# with the same brew formulae installed, so without this step the app dies at
# launch on every other Mac with "Library not loaded". Walk the link graph,
# copy every non-system dylib into Contents/Frameworks, and rewrite the load
# commands to @rpath.
echo "  bundling third-party dylibs..."
FRAMEWORKS_DIR="${CONTENTS_DIR}/Frameworks"
mkdir -p "${FRAMEWORKS_DIR}"
BIN="${MACOS_DIR}/${APP_NAME}"

# Direct deps of a Mach-O, minus the leading header line. For a dylib the
# first entry is its own install name, filtered out later by "seen".
deps_of() {
    otool -L "$1" | tail -n +2 | awk '{print $1}'
}

rpaths_of() {
    otool -l "$1" | awk '/LC_RPATH/{f=1} f && / path /{print $2; f=0}'
}

# Turn a dependency string into a real file to copy, or "" if it's a system
# library we should leave alone. Absolute paths are used as-is. Loader-relative
# paths (some brew kegs reference their siblings as @rpath/libfoo.dylib) are
# resolved against the *original* location of the binary that names them --
# passed in as owner_dir -- and against that binary's own LC_RPATHs.
resolve_dep() {
    dep="$1"
    owner="$2"
    owner_dir="$3"
    case "${dep}" in
        /usr/lib/*|/System/*)
            echo "" ;;
        @executable_path/*|@loader_path/*|@rpath/*)
            rel="${dep#*/}"
            if [ -f "${owner_dir}/${rel}" ]; then
                echo "${owner_dir}/${rel}"
                return
            fi
            for rp in $(rpaths_of "${owner}"); do
                case "${rp}" in
                    @loader_path*|@executable_path*) cand="${owner_dir}/${rp#*/}/${rel}" ;;
                    *) cand="${rp}/${rel}" ;;
                esac
                if [ -f "${cand}" ]; then
                    echo "${cand}"
                    return
                fi
            done
            echo "" ;;
        *)
            echo "${dep}" ;;
    esac
}

# Breadth-first walk so transitive deps (ffmpeg alone pulls in ~20 codecs) get
# copied too, each exactly once. bash 3.2 on macOS has no associative arrays,
# so "seen" is a space-delimited string of basenames and each queue entry
# carries its own original directory after a "|".
queue=("${BIN}|${MACOS_DIR}")
seen=" "
while [ ${#queue[@]} -gt 0 ]; do
    entry="${queue[0]}"
    queue=("${queue[@]:1}")
    current="${entry%%|*}"
    current_dir="${entry#*|}"
    for dep in $(deps_of "${current}"); do
        src="$(resolve_dep "${dep}" "${current}" "${current_dir}")"
        if [ -z "${src}" ]; then continue; fi
        name="$(basename "${src}")"
        case "${seen}" in *" ${name} "*) continue ;; esac
        seen="${seen}${name} "
        if [ ! -f "${src}" ]; then
            echo "  warning: dependency not found, skipping: ${dep} (from ${current})"
            continue
        fi
        # cp follows the brew opt/ -> Cellar/ symlink and copies real contents
        cp "${src}" "${FRAMEWORKS_DIR}/${name}"
        chmod u+w "${FRAMEWORKS_DIR}/${name}"
        # Record the source dir, not the bundle dir, so this lib's own
        # loader-relative deps resolve against the keg they came from.
        src_dir="$(cd "$(dirname "${src}")" && pwd)"
        queue+=("${FRAMEWORKS_DIR}/${name}|${src_dir}")
    done
done

# Second pass: now that every basename is known, point every load command at
# the bundled copy.
for lib in "${FRAMEWORKS_DIR}"/*.dylib; do
    install_name_tool -id "@rpath/$(basename "${lib}")" "${lib}"
done
for target in "${FRAMEWORKS_DIR}"/*.dylib "${BIN}"; do
    for dep in $(deps_of "${target}"); do
        case "${dep}" in
            /usr/lib/*|/System/*) continue ;;
        esac
        name="$(basename "${dep}")"
        if [ "${dep}" = "@rpath/${name}" ]; then continue; fi
        if [ -f "${FRAMEWORKS_DIR}/${name}" ]; then
            install_name_tool -change "${dep}" "@rpath/${name}" "${target}"
        fi
    done
done

# Drop absolute LC_RPATH entries inherited from the brew builds. These are
# searched before the ones added below, so leaving them in means @rpath
# resolves to /opt/homebrew on any machine that has it -- silently loading a
# different SDL3 than the one bundled, and masking bundling mistakes here.
strip_absolute_rpaths() {
    rpaths_of "$1" | while read -r rp; do
        case "${rp}" in
            @*) continue ;;
            *) install_name_tool -delete_rpath "${rp}" "$1" 2>/dev/null || true ;;
        esac
    done
}
for lib in "${FRAMEWORKS_DIR}"/*.dylib; do
    strip_absolute_rpaths "${lib}"
    install_name_tool -add_rpath "@loader_path" "${lib}" 2>/dev/null || true
done
strip_absolute_rpaths "${BIN}"
install_name_tool -add_rpath "@executable_path/../Frameworks" "${BIN}" 2>/dev/null || true

# Fail loudly rather than shipping a bundle that only runs on this machine.
echo "  verifying bundle is self-contained..."
UNRESOLVED=0
for target in "${FRAMEWORKS_DIR}"/*.dylib "${BIN}"; do
    for dep in $(deps_of "${target}"); do
        case "${dep}" in
            /usr/lib/*|/System/*) continue ;;
            @rpath/*)
                if [ ! -f "${FRAMEWORKS_DIR}/${dep#@rpath/}" ]; then
                    echo "  ERROR: ${target} needs ${dep}, not in Frameworks/"
                    UNRESOLVED=1
                fi ;;
            *)
                echo "  ERROR: ${target} still references ${dep} outside the bundle"
                UNRESOLVED=1 ;;
        esac
    done
done
if [ "${UNRESOLVED}" -ne 0 ]; then
    echo "bundle is not self-contained; aborting."
    exit 1
fi

# Homebrew's ffmpeg is GPLv3; shipping it would relicense sink (LGPL-2.1).
# The vendored build in vendor/ffmpeg is LGPL-2.1, but a stale build dir or a
# missing PKG_CONFIG_PATH silently falls back to the system one, so check the
# bytes that are actually in the bundle rather than trusting the build config.
if [ -f "${FRAMEWORKS_DIR}/libavcodec.62.dylib" ]; then
    echo "  verifying bundled FFmpeg is LGPL..."
    if strings "${FRAMEWORKS_DIR}/libavcodec.62.dylib" | grep -q -- "--enable-gpl"; then
        echo "  ERROR: bundled FFmpeg is a GPL build. sink is LGPL-2.1 and must"
        echo "         not ship it. Run vendor/build_ffmpeg.sh, then reconfigure"
        echo "         with: rm -rf build && cmake -B build -S ."
        exit 1
    fi
fi

# install_name_tool invalidates code signatures, and arm64 refuses to load an
# unsigned Mach-O. Ad-hoc sign here; signapp.sh re-signs with the Developer ID.
echo "  ad-hoc signing rewritten binaries..."
for lib in "${FRAMEWORKS_DIR}"/*.dylib; do
    codesign --force --sign - "${lib}" 2>/dev/null
done
codesign --force --sign - "${BIN}" 2>/dev/null

echo "  bundled $(ls -1 "${FRAMEWORKS_DIR}" | wc -l | tr -d ' ') dylibs."

# 5. Generate the Info.plist file
echo "  generating Info.plist..."
cat <<EOF > "${CONTENTS_DIR}/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>icon.icns</string>
    <key>CFBundleIdentifier</key>
    <string>com.rainmultimedia.${APP_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.8</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>copyright © 2026 rain multimedia. all rights reserved.</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.developer-tools</string>

    <!-- TCC usage descriptions.
         A terminal runs arbitrary commands, and macOS attributes what those
         child processes do back to the responsible process -- sink. So the
         prompt the user sees says "sink wants to access ..." no matter which
         command actually asked. Without these keys macOS supplies generic
         text, which for a terminal is actively misleading: it reads as though
         sink itself wants the data. Each string says a command requested it,
         so the user can judge against whatever they just ran.

         Declaring a key neither requests nor grants anything; it only supplies
         the wording if a prompt ever happens. Apple's own Terminal.app ships
         none of these, but it is a platform binary with TCC exceptions that
         third-party apps cannot use. -->
    <key>NSDesktopFolderUsageDescription</key>
    <string>A command run in sink is trying to read or write files on your Desktop.</string>
    <key>NSDocumentsFolderUsageDescription</key>
    <string>A command run in sink is trying to read or write files in your Documents folder.</string>
    <key>NSDownloadsFolderUsageDescription</key>
    <string>A command run in sink is trying to read or write files in your Downloads folder.</string>
    <key>NSRemovableVolumeUsageDescription</key>
    <string>A command run in sink is trying to access files on a removable volume.</string>
    <key>NSNetworkVolumesUsageDescription</key>
    <string>A command run in sink is trying to access files on a network volume.</string>
    <key>NSFileProviderDomainUsageDescription</key>
    <string>A command run in sink is trying to access files stored in iCloud Drive or another cloud provider.</string>
    <key>NSAppleEventsUsageDescription</key>
    <string>A command run in sink (such as osascript) is trying to control another application.</string>
    <key>NSSystemAdministrationUsageDescription</key>
    <string>A command run in sink is trying to perform an administrative task.</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>A command run in sink is trying to reach devices on your local network.</string>
    <key>NSCameraUsageDescription</key>
    <string>A command run in sink is trying to use the camera.</string>
    <key>NSMicrophoneUsageDescription</key>
    <string>A command run in sink is trying to use the microphone.</string>
    <key>NSLocationWhenInUseUsageDescription</key>
    <string>A command run in sink is trying to determine your location.</string>
    <key>NSContactsUsageDescription</key>
    <string>A command run in sink is trying to access your contacts.</string>
    <key>NSCalendarsUsageDescription</key>
    <string>A command run in sink is trying to access your calendars.</string>
    <key>NSRemindersUsageDescription</key>
    <string>A command run in sink is trying to access your reminders.</string>
    <key>NSPhotoLibraryUsageDescription</key>
    <string>A command run in sink is trying to access your photo library.</string>
</dict>
</plist>
EOF

echo "successfully packaged ${APP_DIR}!"
echo "you can now launch it by running: open ${APP_DIR}"
