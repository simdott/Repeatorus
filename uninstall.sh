#!/bin/bash

# Repeatorus LV2 plugin uninstall script

set -e

PLUGIN_NAME="repeatorus"

echo "Repeatorus LV2 Plugin Uninstall"
echo "=========================="

# Detect installation type based on sudo
if [ "$EUID" -eq 0 ]; then
    # Running as root (with sudo)
    INSTALL_DIR="/usr/lib/lv2/${PLUGIN_NAME}.lv2"
    SUDO=""
    echo "Uninstalling system-wide version (detected sudo)"
else
    # Running as normal user
    INSTALL_DIR="${HOME}/.lv2/${PLUGIN_NAME}.lv2"
    SUDO=""
    echo "Uninstalling user version"
fi

# Check if plugin is installed
if [ ! -d "$INSTALL_DIR" ]; then
    echo "Error: Plugin not found at $INSTALL_DIR"
    exit 1
fi

# Show what will be removed
echo "The following directory will be removed:"
echo "  $INSTALL_DIR"
echo "Contents:"
ls -la "$INSTALL_DIR"

# Confirm uninstallation
read -p "Proceed with uninstallation? (y/n): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstall cancelled."
    exit 0
fi

# Remove the plugin directory
echo "Removing $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"

# Verify removal
if [ ! -d "$INSTALL_DIR" ]; then
    echo "✓ Plugin successfully uninstalled from $INSTALL_DIR"
else
    echo "✗ Error: Plugin directory still exists"
    exit 1
fi

# Optional: Clean up empty parent directory if user installation
if [ "$EUID" -ne 0 ] && [ -d "${HOME}/.lv2" ]; then
    if [ -z "$(ls -A ${HOME}/.lv2)" ]; then
        read -p "~/.lv2 is empty. Remove it? (y/n): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            rmdir "${HOME}/.lv2"
            echo "✓ Removed empty ~/.lv2 directory"
        fi
    fi
fi

echo "Uninstall complete."
