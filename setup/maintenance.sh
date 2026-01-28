#!/bin/bash
# Maintenance helper for read-only Pi
# Temporarily enables write mode for making changes

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo bash maintenance.sh"
    exit 1
fi

case "$1" in
    enable)
        echo "Disabling overlay filesystem (enabling writes)..."
        raspi-config nonint disable_overlayfs
        echo ""
        echo "Reboot required to enable write mode."
        echo "After reboot, make your changes, then run:"
        echo "  sudo bash maintenance.sh disable"
        echo ""
        read -p "Reboot now? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            reboot
        fi
        ;;
    disable)
        echo "Enabling overlay filesystem (making read-only)..."
        raspi-config nonint enable_overlayfs
        echo ""
        echo "Reboot required to enable read-only mode."
        read -p "Reboot now? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            reboot
        fi
        ;;
    status)
        echo "Checking overlay filesystem status..."
        if mount | grep -q "overlay on / "; then
            echo "Status: READ-ONLY (overlay active)"
        else
            echo "Status: WRITABLE (overlay disabled)"
        fi
        echo ""
        echo "Services:"
        systemctl status usb-gadget.service --no-pager || true
        echo ""
        systemctl status airbridge.service --no-pager || true
        ;;
    logs)
        echo "Airbridge service logs (last 50 lines):"
        journalctl -u airbridge.service -n 50 --no-pager
        ;;
    *)
        echo "Usage: sudo bash maintenance.sh {enable|disable|status|logs}"
        echo ""
        echo "  enable  - Disable read-only mode (requires reboot)"
        echo "  disable - Enable read-only mode (requires reboot)"
        echo "  status  - Show current mode and service status"
        echo "  logs    - Show airbridge service logs"
        ;;
esac
