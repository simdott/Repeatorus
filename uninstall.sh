#!/bin/sh

# Repeatorus LV2 plugin uninstall script

set -e

PLUGIN_NAME="repeatorus"

echo "Repeatorus LV2 Plugin Uninstall"
echo "=========================="

# Detect installation type
if [ "$(id -u)" -eq 0 ]; then
    INSTALL_DIR="/usr/lib/lv2/${PLUGIN_NAME}.lv2"
    echo "Uninstalling system-wide version"
else
    INSTALL_DIR="${HOME}/.lv2/${PLUGIN_NAME}.lv2"
    echo "Uninstalling user version"
fi

# Check if plugin is installed
if [ ! -d "$INSTALL_DIR" ]; then
    echo "Error: Plugin not found at $INSTALL_DIR"
    exit 1
fi

# Show what will be removed
echo "Removing: $INSTALL_DIR"

# Remove the plugin directory
rm -rf "$INSTALL_DIR"

# Clean up empty parent directory if user installation
if [ "$(id -u)" -ne 0 ] && [ -d "${HOME}/.lv2" ]; then
    if [ -z "$(ls -A ${HOME}/.lv2 2>/dev/null)" ]; then
        rmdir "${HOME}/.lv2" 2>/dev/null || true
        echo "Removed empty ~/.lv2 directory"
    fi
fi

echo "Uninstall complete."
