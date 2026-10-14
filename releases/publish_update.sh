#!/bin/bash
# publish_update.sh - Offer a GitHub release to existing installs as an update.
#
# Usage: releases/publish_update.sh <tag>        e.g. releases/publish_update.sh v0.11-beta
#
# Run it last, after package.sh, signapp.sh and makedmg.sh, once the release
# exists on GitHub with its notes written. It:
#
#   1. reads the version out of the app inside sink.dmg -- the DMG being
#      published, not whatever sink.app happens to be lying around;
#   2. signs sink.dmg with sink's update key (EdDSA, kept in the Keychain);
#   3. writes appcast.xml describing it, with the release's own notes as the
#      text Sparkle shows, and signs the feed too (Info.plist sets
#      SURequireSignedFeed, so an unsigned feed is ignored);
#   4. uploads both to the release, replacing any sink.dmg already there, so
#      the signature always matches the bytes people download.
#
# Installed copies of sink read the feed from releases/latest/download/, which
# GitHub points at whichever release is marked latest -- so a draft or a
# pre-release is never offered, and publishing a newer release retires this
# feed automatically.
#
# The signing key was created with
#     vendor/sparkle/bin/generate_keys --account com.rainmultimedia.sink
# Losing it means existing installs can never accept another update, so keep a
# backup somewhere safe:
#     vendor/sparkle/bin/generate_keys --account com.rainmultimedia.sink -x <file>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

REPO="skyscapeparadise/sink"
KEY_ACCOUNT="com.rainmultimedia.sink"
DMG="sink.dmg"
APPCAST="appcast.xml"

TAG="${1:-}"
if [ -z "${TAG}" ]; then
    echo "usage: $0 <tag>    (e.g. $0 v0.11-beta)" >&2
    exit 1
fi

command -v gh > /dev/null || { echo "ERROR: gh (GitHub CLI) not found" >&2; exit 1; }
[ -f "${DMG}" ] || { echo "ERROR: ${DMG} not found; run releases/makedmg.sh first" >&2; exit 1; }

./vendor/fetch_sparkle.sh > /dev/null
SIGN_UPDATE="vendor/sparkle/bin/sign_update"

# 1. The release has to exist already, published rather than a draft, and not
# a pre-release -- otherwise releases/latest never points at it.
echo "checking release ${TAG}..."
RELEASE_STATE="$(gh release view "${TAG}" -R "${REPO}" --json isDraft,isPrerelease \
    -q '"\(.isDraft) \(.isPrerelease)"')" \
    || { echo "ERROR: no release ${TAG} on ${REPO}; create it first" >&2; exit 1; }
if [ "${RELEASE_STATE}" != "false false" ]; then
    echo "ERROR: ${TAG} is a draft or a pre-release; installed copies only see" >&2
    echo "       the release GitHub marks latest. Publish it as a full release." >&2
    exit 1
fi
NOTES="$(gh release view "${TAG}" -R "${REPO}" --json body -q .body)"

# 2. Version numbers from the app actually inside the DMG.
echo "reading version from ${DMG}..."
MOUNT="$(mktemp -d)"
cleanup() { hdiutil detach "${MOUNT}" -quiet 2>/dev/null || true; rmdir "${MOUNT}" 2>/dev/null || true; }
trap cleanup EXIT
hdiutil attach "${DMG}" -nobrowse -readonly -mountpoint "${MOUNT}" -quiet
PLIST="${MOUNT}/sink.app/Contents/Info.plist"
plist_value() { /usr/libexec/PlistBuddy -c "Print :$1" "${PLIST}"; }
VERSION="$(plist_value CFBundleVersion)"
SHORT_VERSION="$(plist_value CFBundleShortVersionString)"
MIN_SYSTEM="$(plist_value LSMinimumSystemVersion)"
plist_value SUPublicEDKey > /dev/null 2>&1 \
    || { echo "ERROR: sink.app in ${DMG} has no SUPublicEDKey; was it built by package.sh?" >&2; exit 1; }
cleanup
trap - EXIT
echo "  version ${SHORT_VERSION} (build ${VERSION}), macOS ${MIN_SYSTEM}+"

# 3. Sign the DMG. Prints: sparkle:edSignature="..." length="..."
echo "signing ${DMG}..."
ENCLOSURE_SIGNATURE="$("${SIGN_UPDATE}" --account "${KEY_ACCOUNT}" "${DMG}")"

# 4. The feed. Release notes go in as markdown inside CDATA; the only sequence
# that can break out of CDATA is "]]>", so split any that appear.
NOTES_CDATA="${NOTES//]]>/]]]]><![CDATA[>}"
PUB_DATE="$(LC_ALL=C date -u "+%a, %d %b %Y %H:%M:%S +0000")"
cat > "${APPCAST}" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
    <channel>
        <title>sink</title>
        <link>https://github.com/${REPO}</link>
        <item>
            <title>sink ${SHORT_VERSION}</title>
            <pubDate>${PUB_DATE}</pubDate>
            <sparkle:version>${VERSION}</sparkle:version>
            <sparkle:shortVersionString>${SHORT_VERSION}</sparkle:shortVersionString>
            <sparkle:minimumSystemVersion>${MIN_SYSTEM}</sparkle:minimumSystemVersion>
            <sparkle:fullReleaseNotesLink>https://github.com/${REPO}/releases</sparkle:fullReleaseNotesLink>
            <description sparkle:format="markdown"><![CDATA[${NOTES_CDATA}]]></description>
            <enclosure url="https://github.com/${REPO}/releases/download/${TAG}/${DMG}"
                       type="application/octet-stream"
                       ${ENCLOSURE_SIGNATURE} />
        </item>
    </channel>
</rss>
EOF
echo "signing ${APPCAST}..."
"${SIGN_UPDATE}" --account "${KEY_ACCOUNT}" "${APPCAST}" > /dev/null

# 5. Upload. This is what makes the update live for everyone running sink,
# so confirm first.
echo
echo "ready to publish sink ${SHORT_VERSION} as an update via ${TAG}:"
echo "  uploads ${DMG} (replacing the one on the release) and ${APPCAST}"
printf "go ahead? [y/N] "
read -r ANSWER
case "${ANSWER}" in
    y|Y|yes) ;;
    *) echo "not published. ${APPCAST} is left here to look over."; exit 1 ;;
esac
gh release upload "${TAG}" -R "${REPO}" "${DMG}" "${APPCAST}" --clobber
rm -f "${APPCAST}"

echo
echo "published. installed copies will see ${SHORT_VERSION} on their next check"
echo "(daily), or right away from sink > Check for Updates…"
