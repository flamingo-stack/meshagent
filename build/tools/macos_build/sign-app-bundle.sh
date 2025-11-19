#!/bin/bash
# Sign macOS Application Bundle with hardened runtime and entitlements
# This script signs .app bundles for distribution

set -e  # Exit on any error

BUNDLE_PATH="$1"
SIGN_CERT="${MACOS_SIGN_CERT}"

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ENTITLEMENTS_PATH="$PROJECT_ROOT/build/macos_bundle/meshagent.entitlements"

if [ -z "$BUNDLE_PATH" ]; then
    echo "Usage: $0 <bundle_path>"
    echo ""
    echo "Environment variables:"
    echo "  MACOS_SIGN_CERT - Code signing certificate identity (required)"
    echo "                    Example: \"Developer ID Application: Name (TEAMID)\""
    echo ""
    echo "Example:"
    echo "  export MACOS_SIGN_CERT=\"Developer ID Application: Name (TEAMID)\""
    echo "  $0 build/output/MeshAgent.app"
    exit 1
fi

if [ ! -d "$BUNDLE_PATH" ]; then
    echo "Error: Bundle not found: $BUNDLE_PATH"
    exit 1
fi

if [ -z "$SIGN_CERT" ]; then
    echo "Error: MACOS_SIGN_CERT environment variable not set"
    echo "Please set it to your Developer ID Application certificate identity"
    echo "Example: export MACOS_SIGN_CERT=\"Developer ID Application: Name (TEAMID)\""
    exit 1
fi

if [ ! -f "$ENTITLEMENTS_PATH" ]; then
    echo "Error: Entitlements file not found: $ENTITLEMENTS_PATH"
    exit 1
fi

echo "Signing macOS application bundle"
echo "  Bundle: $BUNDLE_PATH"
echo "  Certificate: $SIGN_CERT"
echo "  Entitlements: $ENTITLEMENTS_PATH"

# Verify certificate exists in keychain
if ! security find-identity -v -p codesigning | grep -q "$SIGN_CERT"; then
    echo "Error: Certificate not found in keychain: $SIGN_CERT"
    echo ""
    echo "Available certificates:"
    security find-identity -v -p codesigning
    exit 1
fi

# Sign the bundle with entitlements
echo ""
echo "Signing bundle..."
codesign --sign "$SIGN_CERT" \
         --entitlements "$ENTITLEMENTS_PATH" \
         --options runtime \
         --timestamp \
         --deep \
         --force \
         "$BUNDLE_PATH"

if [ $? -eq 0 ]; then
    echo "✓ Bundle signed successfully"
else
    echo "✗ Signing failed"
    exit 1
fi

# Verify the signature
echo ""
echo "Verifying signature..."
codesign -dvvv --deep --strict "$BUNDLE_PATH" 2>&1 | head -20

if codesign --verify --deep --strict "$BUNDLE_PATH" 2>/dev/null; then
    echo "✓ Signature verification passed"
else
    echo "✗ Signature verification failed"
    exit 1
fi

# Check if bundle is universal and extract slices if so
EXECUTABLE="$BUNDLE_PATH/Contents/MacOS/meshagent"
if [ -f "$EXECUTABLE" ]; then
    if lipo -info "$EXECUTABLE" 2>/dev/null | grep -q "Non-fat file"; then
        echo ""
        echo "Single architecture bundle (no extraction needed)"
        lipo -info "$EXECUTABLE"
    elif lipo -info "$EXECUTABLE" 2>/dev/null | grep -q "Architectures in the fat file"; then
        echo ""
        echo "Universal bundle detected"
        lipo -info "$EXECUTABLE"

        # Get parent directory of bundle
        BUNDLE_DIR="$(dirname "$BUNDLE_PATH")"
        BUNDLE_NAME="$(basename "$BUNDLE_PATH" .app)"

        # Extract x86_64 bundle
        if lipo -info "$EXECUTABLE" | grep -q "x86_64"; then
            X86_BUNDLE="$BUNDLE_DIR/${BUNDLE_NAME}-x86_64.app"
            echo ""
            echo "Extracting x86_64 bundle to: $X86_BUNDLE"
            cp -R "$BUNDLE_PATH" "$X86_BUNDLE"
            lipo "$EXECUTABLE" -thin x86_64 -output "$X86_BUNDLE/Contents/MacOS/meshagent"
            echo "✓ x86_64 bundle created"
        fi

        # Extract arm64 bundle
        if lipo -info "$EXECUTABLE" | grep -q "arm64"; then
            ARM_BUNDLE="$BUNDLE_DIR/${BUNDLE_NAME}-arm64.app"
            echo ""
            echo "Extracting arm64 bundle to: $ARM_BUNDLE"
            cp -R "$BUNDLE_PATH" "$ARM_BUNDLE"
            lipo "$EXECUTABLE" -thin arm64 -output "$ARM_BUNDLE/Contents/MacOS/meshagent"
            echo "✓ arm64 bundle created"
        fi
    fi
fi

echo ""
echo "Bundle signing complete: $BUNDLE_PATH"
echo ""
echo "Next steps:"
echo "  1. Notarize: ./build/tools/macos_build/notarize-app-bundle.sh $BUNDLE_PATH"
echo "  2. Verify: spctl -a -vvv -t install $BUNDLE_PATH"
