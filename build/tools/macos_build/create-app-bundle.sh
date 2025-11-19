#!/bin/bash
# Create macOS Application Bundle from binary
# This script takes a standalone binary and packages it into a .app bundle

set -e  # Exit on any error

BINARY_PATH="$1"
BUNDLE_NAME="${2:-MeshAgent.app}"
BUNDLE_ID="${3:-meshagent}"
BUILD_TIMESTAMP="${4:-$(date +%y.%m.%d.%H.%M.%S)}"

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ -z "$BINARY_PATH" ]; then
    echo "Usage: $0 <binary_path> [bundle_name] [bundle_id] [build_timestamp]"
    echo ""
    echo "Arguments:"
    echo "  binary_path      Path to the compiled meshagent binary (required)"
    echo "  bundle_name      Name of the .app bundle (default: MeshAgent.app)"
    echo "  bundle_id        Bundle identifier (default: meshagent)"
    echo "  build_timestamp  Version timestamp (default: current date/time)"
    echo ""
    echo "Example:"
    echo "  $0 build/output/meshagent_osx-arm-64 MeshAgent.app com.meshcentral.meshagent"
    exit 1
fi

if [ ! -f "$BINARY_PATH" ]; then
    echo "Error: Binary not found: $BINARY_PATH"
    exit 1
fi

echo "Creating app bundle: $BUNDLE_NAME"
echo "  Binary: $BINARY_PATH"
echo "  Bundle ID: $BUNDLE_ID"
echo "  Version: $BUILD_TIMESTAMP"
echo "  Project root: $PROJECT_ROOT"

# Detect if binary is universal
if lipo -info "$BINARY_PATH" 2>/dev/null | grep -q "Architectures in the fat file"; then
    echo "  Architecture: Universal ($(lipo -info "$BINARY_PATH" | sed 's/.*: //'))"
elif lipo -info "$BINARY_PATH" 2>/dev/null | grep -q "Non-fat file"; then
    ARCH=$(lipo -info "$BINARY_PATH" | sed 's/.*: //')
    echo "  Architecture: $ARCH"
else
    echo "  Architecture: Unknown"
fi

# Create bundle structure
# Standard macOS bundle layout: Contents/{MacOS,Resources}
mkdir -p "$BUNDLE_NAME/Contents/MacOS"
mkdir -p "$BUNDLE_NAME/Contents/Resources"

# Copy binary
cp "$BINARY_PATH" "$BUNDLE_NAME/Contents/MacOS/meshagent"
chmod +x "$BUNDLE_NAME/Contents/MacOS/meshagent"
echo "  Copied binary to Contents/MacOS/meshagent"

# Generate Info.plist
TEMPLATE_PATH="$PROJECT_ROOT/build/resources/Info/bundle/_Info.plist"
if [ ! -f "$TEMPLATE_PATH" ]; then
    echo "Error: Info.plist template not found: $TEMPLATE_PATH"
    exit 1
fi

sed -e "s/BUNDLE_IDENTIFIER/$BUNDLE_ID/g" \
    -e "s/BUILD_TIMESTAMP/$BUILD_TIMESTAMP/g" \
    "$TEMPLATE_PATH" > "$BUNDLE_NAME/Contents/Info.plist"
echo "  Generated Info.plist"

# Copy icon
ICON_PATH="$PROJECT_ROOT/build/tools/macos_build/icon/meshagent.icns"
if [ -f "$ICON_PATH" ]; then
    cp "$ICON_PATH" "$BUNDLE_NAME/Contents/Resources/meshagent.icns"
    echo "  Copied icon: meshagent.icns"
else
    echo "  Warning: Icon not found at $ICON_PATH"
    echo "  Bundle will be created without icon"
fi

# Optional: Copy modules if needed
# Uncomment if JavaScript modules should be bundled
# if [ -d "$PROJECT_ROOT/modules" ]; then
#     mkdir -p "$BUNDLE_NAME/Contents/Resources/modules"
#     cp -r "$PROJECT_ROOT/modules/"* "$BUNDLE_NAME/Contents/Resources/modules/"
#     echo "  Copied modules to Contents/Resources/modules/"
# fi

# Create PkgInfo file (optional but recommended for compatibility)
echo -n "APPLMESH" > "$BUNDLE_NAME/Contents/PkgInfo"
echo "  Created PkgInfo"

echo ""
echo "Bundle created successfully: $BUNDLE_NAME"
echo ""
echo "Bundle structure:"
ls -lR "$BUNDLE_NAME" | head -20
echo ""
echo "To test:"
echo "  $BUNDLE_NAME/Contents/MacOS/meshagent --version"
echo ""
echo "To launch:"
echo "  open $BUNDLE_NAME"
echo ""
echo "To install:"
echo "  cp -R $BUNDLE_NAME /Applications/"
