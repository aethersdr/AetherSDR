#!/bin/bash
set -euo pipefail

# Build MasterSDR macOS installer (.pkg)
# Usage: ./packaging/macos/build-installer.sh [build-dir]

BUILD_DIR="${1:-build}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="${BUILD_DIR}/pkg-staging"
VERSION=$(grep 'project(MasterSDR' CMakeLists.txt | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")

echo "=== Building MasterSDR macOS installer v${VERSION} ==="

# 1. Build app
cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${BUILD_DIR}" -j$(sysctl -n hw.ncpu)

# 1b. Build HAL plugin (separate build because libASPL FetchContent conflicts with main Ninja build)
HAL_BUILD="${BUILD_DIR}-hal"
if [ -f "hal-plugin/CMakeLists.txt" ]; then
    echo "--- Building HAL plugin ---"
    cmake -B "${HAL_BUILD}" -S hal-plugin -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "${HAL_BUILD}" -j$(sysctl -n hw.ncpu)
fi

# 2. Prepare staging
rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}/app" "${PKG_DIR}/hal" "${PKG_DIR}/scripts"

# Copy app bundle
cp -R "${BUILD_DIR}/MasterSDR.app" "${PKG_DIR}/app/"

# Copy HAL plugin (check both possible build locations)
if [ -d "${HAL_BUILD}/MasterSDRDAX.driver" ]; then
    cp -R "${HAL_BUILD}/MasterSDRDAX.driver" "${PKG_DIR}/hal/"
elif [ -d "${BUILD_DIR}/MasterSDRDAX.driver" ]; then
    cp -R "${BUILD_DIR}/MasterSDRDAX.driver" "${PKG_DIR}/hal/"
else
    echo "WARNING: HAL plugin not found"
    echo "The installer will only contain the app."
fi

# 3. Create postinstall script
cat > "${PKG_DIR}/scripts/postinstall" << 'POSTINSTALL'
#!/bin/bash
# Restart CoreAudio daemon so the new virtual audio device appears
launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || true
exit 0
POSTINSTALL
chmod +x "${PKG_DIR}/scripts/postinstall"

# 4. Build component packages
pkgbuild \
    --root "${PKG_DIR}/app" \
    --install-location /Applications \
    --identifier com.mastersdr.app \
    --version "${VERSION}" \
    "${PKG_DIR}/MasterSDR-app.pkg"

if [ -d "${PKG_DIR}/hal/MasterSDRDAX.driver" ]; then
    pkgbuild \
        --root "${PKG_DIR}/hal" \
        --install-location "/Library/Audio/Plug-Ins/HAL" \
        --identifier com.mastersdr.dax \
        --version "${VERSION}" \
        --scripts "${PKG_DIR}/scripts" \
        "${PKG_DIR}/MasterSDR-dax.pkg"
fi

# 5. Create distribution XML
cat > "${PKG_DIR}/Distribution.xml" << DISTXML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>MasterSDR ${VERSION}</title>
    <welcome file="welcome.html"/>
    <options customize="allow" require-scripts="false"/>

    <script>
    function daxDriverInstalled() {
        return system.files.fileExistsAtPath('/Library/Audio/Plug-Ins/HAL/MasterSDRDAX.driver');
    }
    function daxDriverNeedsUpdate() {
        // Always offer update if installed version differs
        var plist = system.files.plistAtPath('/Library/Audio/Plug-Ins/HAL/MasterSDRDAX.driver/Contents/Info.plist');
        if (plist) {
            var installed = plist['CFBundleShortVersionString'] || '0';
            return (installed !== '${VERSION}');
        }
        return true;
    }
    </script>

    <choices-outline>
        <line choice="app"/>
        <line choice="dax"/>
    </choices-outline>

    <choice id="app" title="MasterSDR Application"
            description="SmartSDR-compatible client for FlexRadio">
        <pkg-ref id="com.mastersdr.app"/>
    </choice>

    <choice id="dax" title="DAX Virtual Audio Driver"
            description="Virtual audio devices for digital mode apps (WSJT-X, fldigi, etc.). Uncheck if already installed."
            selected="!daxDriverInstalled() || daxDriverNeedsUpdate()"
            tooltip="Currently installed: will skip unless a newer version is available">
        <pkg-ref id="com.mastersdr.dax"/>
    </choice>

    <pkg-ref id="com.mastersdr.app" version="${VERSION}" onConclusion="none">MasterSDR-app.pkg</pkg-ref>
    <pkg-ref id="com.mastersdr.dax" version="${VERSION}" onConclusion="none">MasterSDR-dax.pkg</pkg-ref>
</installer-gui-script>
DISTXML

# 6. Create welcome HTML
mkdir -p "${PKG_DIR}/resources"
cat > "${PKG_DIR}/resources/welcome.html" << 'WELCOME'
<html><body>
<h1>MasterSDR</h1>
<p>A native SmartSDR client for FlexRadio on macOS.</p>
<p>This installer includes:</p>
<ul>
<li><b>MasterSDR.app</b> — the main application</li>
<li><b>DAX Virtual Audio Driver</b> — creates virtual audio devices for digital mode apps (WSJT-X, fldigi, VARA, etc.)</li>
</ul>
<p>After installation, the CoreAudio daemon will restart automatically to register the new audio devices.</p>
</body></html>
WELCOME

# 7. Build product archive
productbuild \
    --distribution "${PKG_DIR}/Distribution.xml" \
    --resources "${PKG_DIR}/resources" \
    --package-path "${PKG_DIR}" \
    "${BUILD_DIR}/MasterSDR-${VERSION}-macOS.pkg"

echo ""
echo "=== Installer created: ${BUILD_DIR}/MasterSDR-${VERSION}-macOS.pkg ==="
