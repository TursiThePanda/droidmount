#!/bin/zsh
# Build Droidmount.app and the DMG from a clean checkout.
#
# Needs (build machine only): Xcode Command Line Tools, Homebrew libusb + cmake + pkg-config,
# and macFUSE 5.5+ installed (for libfuse3 headers/library). Users of the DMG need none
# of this.
#
#   scripts/build.sh            -> build/Droidmount.app and build/Droidmount-<version>.dmg
set -euo pipefail

ROOT=${0:A:h:h}
BUILD=$ROOT/build
VERSION=${DROIDMOUNT_VERSION:-$(<"$ROOT/VERSION")}   # override only for update tests
ARCH=arm64

LIBUSB_VER=1.0.30
LIBUSB_URL=https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2
LIBUSB_SHA256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf

LIBMTP_VER=1.1.23
LIBMTP_URL=https://downloads.sourceforge.net/project/libmtp/libmtp/$LIBMTP_VER/libmtp-$LIBMTP_VER.tar.gz
LIBMTP_SHA256=74a2b6e8cb4a0304e95b995496ea3ac644c29371649b892b856e22f12a0bdeed

SPARKLE_VER=2.10.1
SPARKLE_SHA256=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
# Update feed; tests point it at a local server with DROIDMOUNT_FEED_URL.
FEED_URL=${DROIDMOUNT_FEED_URL:-https://github.com/TursiThePanda/droidmount/releases/latest/download/appcast.xml}

MACFUSE_VER=5.5.0
MACFUSE_DMG=macfuse-$MACFUSE_VER.dmg
MACFUSE_SHA256=92c21ef2db4199bcf5bddc8e6efb57b23ecb9715f74ab38cba2c77ab226d3830

say() { print -P "%F{cyan}==>%f $*"; }
die() { print -P "%F{red}error:%f $*" >&2; exit 1; }
sha_ok() { [[ "$(shasum -a 256 "$1" | cut -d' ' -f1)" == "$2" ]]; }

[[ -d /usr/local/include/fuse3 ]] || die "macFUSE 5.5+ must be installed on the build machine"
for t in cmake pkg-config swiftc; do command -v $t >/dev/null || die "$t not found"; done
mkdir -p "$BUILD/downloads"
MINOS=26.0
export MACOSX_DEPLOYMENT_TARGET=$MINOS

# --- libusb (static, built for macOS 26 so the app runs there too) --------------------
USB=$BUILD/libusb-install
if [[ ! -f "$USB/lib/libusb-1.0.a" ]]; then
    say "libusb $LIBUSB_VER"
    tbz=$BUILD/downloads/libusb-$LIBUSB_VER.tar.bz2
    [[ -f $tbz ]] || curl -fsSL -o "$tbz" "$LIBUSB_URL"
    sha_ok "$tbz" $LIBUSB_SHA256 || die "libusb checksum mismatch"
    rm -rf "$BUILD/libusb" && mkdir -p "$BUILD/libusb"
    tar -xjf "$tbz" -C "$BUILD/libusb" --strip-components 1
    (cd "$BUILD/libusb" && ./configure --prefix="$USB" --enable-static --disable-shared \
        CFLAGS="-O2 -mmacosx-version-min=$MINOS" >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
export PKG_CONFIG_PATH=$USB/lib/pkgconfig

# --- libmtp (static) ------------------------------------------------------------------
if [[ ! -f "$BUILD/libmtp/src/.libs/libmtp.a" ]]; then
    say "libmtp $LIBMTP_VER"
    tgz=$BUILD/downloads/libmtp-$LIBMTP_VER.tar.gz
    [[ -f $tgz ]] || curl -fsSL -o "$tgz" "$LIBMTP_URL"
    sha_ok "$tgz" $LIBMTP_SHA256 || die "libmtp checksum mismatch"
    rm -rf "$BUILD/libmtp" && mkdir -p "$BUILD/libmtp"
    tar -xzf "$tgz" -C "$BUILD/libmtp" --strip-components 1
    (cd "$BUILD/libmtp" && ./configure --disable-mtpz --enable-static --disable-shared \
        CFLAGS="-O2 -mmacosx-version-min=$MINOS" >/dev/null && make -C src -j8 >/dev/null)
fi

# --- filesystem ---------------------------------------------------------------------
say "simple-mtpfs (Droidmount filesystem)"
cmake -S "$ROOT/fs" -B "$BUILD/fs" -DCMAKE_BUILD_TYPE=Release -DLIBMTP_SRC="$BUILD/libmtp" \
    -DLIBUSB_ROOT="$USB" >/dev/null
cmake --build "$BUILD/fs" -j8 >/dev/null

# --- helpers ------------------------------------------------------------------------
say "watcher and probe"
mkdir -p "$BUILD/bin"
cc -O2 -Wall -mmacosx-version-min=26.0 -o "$BUILD/bin/android-automount" \
    "$ROOT/watcher/android-automount.c" -framework IOKit -framework CoreFoundation
cc -O2 -Wall -mmacosx-version-min=26.0 -o "$BUILD/bin/fskit-probe" "$ROOT/probe/fskit-probe.c" \
    -I/usr/local/include/fuse3 -L/usr/local/lib -lfuse3

# --- Sparkle (update framework) -----------------------------------------------------
SPK=$BUILD/sparkle
if [[ ! -d "$SPK/Sparkle.framework" ]]; then
    say "Sparkle $SPARKLE_VER"
    txz=$BUILD/downloads/Sparkle-$SPARKLE_VER.tar.xz
    [[ -f $txz ]] || curl -fsSL -o "$txz" \
        "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VER/Sparkle-$SPARKLE_VER.tar.xz"
    sha_ok "$txz" $SPARKLE_SHA256 || die "Sparkle checksum mismatch"
    rm -rf "$SPK" && mkdir -p "$SPK"
    tar -xJf "$txz" -C "$SPK"
fi

# --- app ----------------------------------------------------------------------------
say "Droidmount app"
swiftc -O -parse-as-library -target $ARCH-apple-macos26.0 "$ROOT"/app/Sources/*.swift \
    -F "$SPK" -framework Sparkle -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
    -o "$BUILD/bin/Droidmount"

# --- macFUSE installer (bundled; its licence allows this for non-commercial software) --
mf=$BUILD/downloads/$MACFUSE_DMG
if [[ ! -f $mf ]] || ! sha_ok "$mf" $MACFUSE_SHA256; then
    say "macFUSE $MACFUSE_VER"
    curl -fsSL -o "$mf" "https://github.com/macfuse/macfuse/releases/download/macfuse-$MACFUSE_VER/$MACFUSE_DMG" || {
        # github.com release downloads sometimes return 504; the API path still works
        asset=$(curl -fsSL "https://api.github.com/repos/macfuse/macfuse/releases/tags/macfuse-$MACFUSE_VER" \
            | python3 -I -c "import json,sys;print([a['url'] for a in json.load(sys.stdin)['assets'] if a['name']=='$MACFUSE_DMG'][0])")
        curl -fsSL -H 'Accept: application/octet-stream' -o "$mf" "$asset"
    }
    sha_ok "$mf" $MACFUSE_SHA256 || die "macFUSE checksum mismatch"
fi

# --- bundle -------------------------------------------------------------------------
say "Droidmount.app"
APP=$BUILD/Droidmount.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Helpers" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
ditto "$SPK/Sparkle.framework" "$APP/Contents/Frameworks/Sparkle.framework"
cp "$BUILD/bin/Droidmount" "$APP/Contents/MacOS/"
cp "$BUILD/fs/simple-mtpfs" "$BUILD/bin/android-automount" "$BUILD/bin/fskit-probe" "$APP/Contents/Helpers/"
sed -e "s/@VERSION@/$VERSION/g" -e "s|@FEED_URL@|$FEED_URL|g" "$ROOT/app/Info.plist" > "$APP/Contents/Info.plist"
# a build number that grows with every commit, so macOS re-reads the icon (it caches icons
# per bundle id and version)
BUILDNUM=$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 0)
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION.$BUILDNUM" "$APP/Contents/Info.plist"
# Icon: the layered Icon Composer icon (follows light/dark/clear/tinted) needs Xcode's
# actool; without Xcode the plain light .icns is used.
if xcrun --find actool >/dev/null 2>&1; then
    rm -rf "$BUILD/actool" && mkdir -p "$BUILD/actool"
    xcrun actool "$ROOT/app/AppIcon.icon" --compile "$BUILD/actool" --platform macosx \
        --minimum-deployment-target $MINOS --app-icon AppIcon --target-device mac \
        --output-partial-info-plist "$BUILD/actool/partial.plist" >/dev/null
    cp "$BUILD/actool/Assets.car" "$BUILD/actool/AppIcon.icns" "$APP/Contents/Resources/"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconName string AppIcon" "$APP/Contents/Info.plist"
else
    say "actool not found (no Xcode): using the plain light icon"
    cp "$ROOT/app/AppIcon.icns" "$APP/Contents/Resources/"
fi

mnt=$(mktemp -d)
hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$mf" >/dev/null
pkgutil --check-signature "$mnt/Install macFUSE.pkg" | grep -q "Developer ID Installer: Benjamin Fleischer" \
    || { hdiutil detach "$mnt" -quiet; die "unexpected macFUSE package signature"; }
cp "$mnt/Install macFUSE.pkg" "$APP/Contents/Resources/"
textutil -convert txt -output "$APP/Contents/Resources/macFUSE-LICENSE.txt" "$mnt/License.rtf"
hdiutil detach "$mnt" -quiet
cp "$ROOT/LICENSE" "$ROOT/NOTICE.md" "$APP/Contents/Resources/"

# ad-hoc signature (no Apple Developer account); the bundled macFUSE pkg keeps its own.
# Extended attributes (quarantine, provenance) make codesign refuse the bundle.
xattr -cr "$APP"
codesign --force --sign - "$APP/Contents/Helpers/"* >/dev/null
# Sparkle checks that an update is signed "the same" as the running app. An ad-hoc
# signature's default requirement is the cdhash, which changes with every build, so pin
# the requirement to the bundle identifier instead.
codesign --force --sign - -r='designated => identifier "io.github.tursithepanda.droidmount"' "$APP" >/dev/null

# --- DMG ----------------------------------------------------------------------------
say "DMG"
stage=$(mktemp -d)
cp -R "$APP" "$stage/"
ln -s /Applications "$stage/Applications"
cp "$ROOT/docs/First launch.txt" "$stage/"
dmg=$BUILD/Droidmount-$VERSION.dmg
rm -f "$dmg"
hdiutil create -volname "Droidmount $VERSION" -srcfolder "$stage" -format UDZO -ov "$dmg" >/dev/null
rm -rf "$stage"
# checksum file names only the file (no local path)
(cd "$BUILD" && shasum -a 256 "${dmg:t}" | tee "${dmg:t}.sha256")
say "done: $dmg"
