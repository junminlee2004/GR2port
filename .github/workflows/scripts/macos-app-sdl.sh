#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

# Makes "Gravity Rush 2™.app" in the current folder from a macOS build: the executable with the
# Vulkan loader and KosmicKrisp beside it, KosmicKrisp's manifest and recompiled mode's generator
# in the resources, the frameworks and plugins of Qt that the launcher needs, and the icon.
# Usage: macos-app-sdl.sh [build folder], by default $GITHUB_WORKSPACE/build.

set -e

source="$(cd "$(dirname "$0")/../../.." && pwd)"
build="${1:-$GITHUB_WORKSPACE/build}"
app="Gravity Rush 2™.app"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$build/src/dist/Info.plist" "$app/Contents/Info.plist"
cp "$build/gr2port" "$app/Contents/MacOS/"

# The icon, from the 256-pixel picture the other systems use.
iconset="$(mktemp -d)/gr2port.iconset"
mkdir -p "$iconset"
for size in 16 32 128 256; do
	sips -z $size $size "$source/src/dist/gr2port.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
	sips -z $((size * 2)) $((size * 2)) "$source/src/dist/gr2port.png" \
		--out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/gr2port.icns"
/usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string gr2port" "$app/Contents/Info.plist"

# Qt's frameworks and plugins go into the bundle, and the executable finds them there. The Vulkan
# files come after, and the search path that finds them beside the executable: macdeployqt reads
# every file beside the executable as a library, and stops at a search path that is
# @executable_path alone.
install_name_tool -delete_rpath @executable_path "$app/Contents/MacOS/gr2port"
macdeployqt "$app"
install_name_tool -add_rpath @executable_path "$app/Contents/MacOS/gr2port"
cp "$build/libvulkan.dylib" "$build/libvulkan_kosmickrisp.dylib" "$app/Contents/MacOS/"

# The Vulkan loader looks for drivers in the bundle's resources first. Only code may sit beside
# the executable: a zip drops the signature codesign gives any other file there.
icd="$app/Contents/Resources/vulkan/icd.d"
mkdir -p "$icd"
sed 's|"\./libvulkan_kosmickrisp\.dylib"|"../../../MacOS/libvulkan_kosmickrisp.dylib"|' \
	"$build/kosmickrisp_mesa_icd.json" >"$icd/kosmickrisp_mesa_icd.json"
grep -q '"../../../MacOS/libvulkan_kosmickrisp.dylib"' "$icd/kosmickrisp_mesa_icd.json"

# The generator that makes recompiled mode's libraries from the player's game on the first start,
# at the places it has in the tree, and digest.txt, a hash of all of it: libraries that another
# generator made are made again.
recompiler="$app/Contents/Resources/recompiler"
zydis="$source/externals/zydis-4.1"
mkdir -p "$recompiler/src/core/recompiled" "$recompiler/third_party" \
	"$recompiler/externals/zydis-4.1/dependencies/zycore"
cp "$source/src/core/recompiled/build-library.py" "$recompiler/src/core/recompiled/"
cp -R "$source/src/core/recompiled/inputs" "$recompiler/src/core/recompiled/"
cp -R "$source/third_party/recompiler" "$source/third_party/cpu-runtime" "$recompiler/third_party/"
cp -R "$zydis/LICENSE" "$zydis/include" "$zydis/src" "$recompiler/externals/zydis-4.1/"
cp -R "$zydis/dependencies/zycore/LICENSE" "$zydis/dependencies/zycore/include" \
	"$zydis/dependencies/zycore/src" "$recompiler/externals/zydis-4.1/dependencies/zycore/"
(cd "$recompiler" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 |
	shasum -a 256 | cut -d ' ' -f 1) >"$recompiler/digest.txt"
# Each library the bundle's code loads must be in the bundle or the system's. A library's own
# name, which otool lists first, does not count.
if find "$app" -type f -print0 | xargs -0 file | grep 'Mach-O' | cut -d: -f1 |
	while IFS= read -r file; do
		otool -L "$file" | sed 1d | grep -vF "$(otool -D "$file" | sed -n 2p) ("
	done | grep -E '^\s+/(opt|usr/local)/'; then
	echo "The bundle still names libraries outside it"
	exit 1
fi

# Apple silicon runs signed code only: macdeployqt's changes need a signature again.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
