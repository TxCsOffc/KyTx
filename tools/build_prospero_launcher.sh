#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$(dirname "$root")/build/kytx"}
bundle="$out/Prospero.app"
mkdir -p "$bundle/Contents/MacOS"
cat > "$bundle/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Prospero</string>
    <key>CFBundleIdentifier</key><string>io.github.luizdusky.prospero</string>
    <key>CFBundleName</key><string>Prospero</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>CFBundleDocumentTypes</key>
    <array><dict>
        <key>CFBundleTypeName</key><string>PlayStation game package</string>
        <key>CFBundleTypeRole</key><string>Viewer</string>
        <key>LSHandlerRank</key><string>Alternate</string>
        <key>CFBundleTypeExtensions</key><array><string>pkg</string><string>fpkg</string></array>
    </dict></array>
</dict>
</plist>
PLIST
xcrun swiftc -parse-as-library -O -target arm64-apple-macos15.0 \
    "$root/src/prospero/Prospero.swift" -o "$bundle/Contents/MacOS/Prospero"
plutil -lint "$bundle/Contents/Info.plist"
codesign --force --sign - "$bundle"
codesign --verify --strict "$bundle"
