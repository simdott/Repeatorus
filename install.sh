#!/bin/bash

# Repeatorus LV2 plugin build and install script

set -e

PLUGIN_URI="<urn:simdott:repeatorus>"
PLUGIN_NAME="repeatorus"
BUILD_DIR="build"

# Detect installation type based on sudo
if [ "$(id -u)" -eq 0 ]; then
    # Running as root (with sudo)
    INSTALL_DIR="/usr/lib/lv2/${PLUGIN_NAME}.lv2"
    SUDO=""
    echo "Installing system-wide (detected sudo)"
else
    # Running as normal user
    INSTALL_DIR="${HOME}/.lv2/${PLUGIN_NAME}.lv2"
    SUDO=""
    echo "Installing for current user only"
fi

# Check for LV2 headers
LV2_HEADER_FOUND=0
for path in /usr/include/lv2/lv2.h \
            /usr/local/include/lv2/lv2.h \
            /usr/include/lv2.h; do
    if [ -f "$path" ]; then
        LV2_HEADER_FOUND=1
        break
    fi
done

if [ $LV2_HEADER_FOUND -eq 0 ]; then
    if pkg-config --exists lv2; then
        LV2_HEADER_FOUND=1
    else
        echo "Error: LV2 headers not found."
        echo "Install lv2-dev (Debian/Ubuntu) or lv2-devel (Fedora)"
        exit 1
    fi
fi

# Get include flags
if pkg-config --exists lv2; then
    LV2_CFLAGS=$(pkg-config --cflags lv2)
else
    LV2_CFLAGS="-I/usr/include/lv2"
fi

# Create build directory
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# Compile
echo "Compiling..."
gcc -std=c99 -fPIC -shared -O2 -march=native -o ${PLUGIN_NAME}.so ../${PLUGIN_NAME}.c \
    $LV2_CFLAGS \
    -lm

cd ..

# Install
echo "Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"
cp $BUILD_DIR/${PLUGIN_NAME}.so "$INSTALL_DIR/"
cp ${PLUGIN_NAME}.ttl "$INSTALL_DIR/"
cp manifest.ttl "$INSTALL_DIR/"

# Clean up
echo "Cleaning up..."
rm -rf $BUILD_DIR
rm -f ${PLUGIN_NAME}.so

echo "Done. Plugin installed to: $INSTALL_DIR"
