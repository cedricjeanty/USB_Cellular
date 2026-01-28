#!/bin/bash
# Setup script to make Pi Zero a standalone read-only appliance
# Run this ONCE on the Pi before deploying

set -e

echo "=== USB Cellular Airbridge - Standalone Setup ==="
echo ""

# Must run as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo bash setup_readonly.sh"
    exit 1
fi

DATA_PARTITION="/dev/mmcblk0p3"
MOUNT_POINT="/mnt/usb_logs"
OUTBOX_DIR="/home/cedric/outbox"

echo "[1/7] Installing required packages..."
apt-get update
apt-get install -y python3-serial python3-yaml python3-flask

echo "[2/7] Creating directories..."
mkdir -p "$MOUNT_POINT"
mkdir -p "$OUTBOX_DIR"
chown cedric:cedric "$OUTBOX_DIR"

echo "[3/7] Installing airbridge services..."
cp /home/cedric/airbridge.service /etc/systemd/system/
cp /home/cedric/airbridge-web.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable airbridge.service
systemctl enable airbridge-web.service

echo "[4/7] Ensuring dwc2 module loads on boot..."
# USB gadget is now loaded by main.py (after boot upload phase)
# We just need to ensure dwc2 driver is available
if ! grep -q "^dwc2" /etc/modules; then
    echo "dwc2" >> /etc/modules
fi

# Remove old usb-gadget service if it exists (main.py now controls this)
if [ -f /etc/systemd/system/usb-gadget.service ]; then
    systemctl disable usb-gadget.service || true
    rm -f /etc/systemd/system/usb-gadget.service
fi

systemctl daemon-reload

echo "[5/7] Configuring tmpfs for volatile directories..."
# Add tmpfs mounts to fstab if not already present
if ! grep -q "tmpfs.*/tmp" /etc/fstab; then
    echo "tmpfs /tmp tmpfs defaults,noatime,nosuid,nodev,size=32M 0 0" >> /etc/fstab
fi
if ! grep -q "tmpfs.*/var/log" /etc/fstab; then
    echo "tmpfs /var/log tmpfs defaults,noatime,nosuid,nodev,size=16M 0 0" >> /etc/fstab
fi
if ! grep -q "tmpfs.*/var/tmp" /etc/fstab; then
    echo "tmpfs /var/tmp tmpfs defaults,noatime,nosuid,nodev,size=16M 0 0" >> /etc/fstab
fi

echo "[6/7] Disabling swap..."
dphys-swapfile swapoff || true
dphys-swapfile uninstall || true
systemctl disable dphys-swapfile || true

echo "[7/7] Enabling read-only root filesystem via raspi-config..."
# Use the built-in overlay filesystem feature
# This creates an overlay on top of root, all writes go to RAM
if command -v raspi-config &> /dev/null; then
    echo "Enabling overlayfs via raspi-config..."
    raspi-config nonint enable_overlayfs
    echo ""
    echo "=========================================="
    echo "READ-ONLY FILESYSTEM ENABLED"
    echo "=========================================="
    echo ""
    echo "After reboot, the root filesystem will be read-only."
    echo "All changes will be lost on power cycle (except DATA partition)."
    echo ""
    echo "To make changes later, you must:"
    echo "  1. sudo raspi-config"
    echo "  2. Performance Options -> Overlay File System -> Disable"
    echo "  3. Reboot, make changes, then re-enable"
    echo ""
else
    echo "WARNING: raspi-config not found. Manual overlay setup required."
    echo "You can enable read-only mode manually with:"
    echo "  sudo raspi-config -> Performance Options -> Overlay File System"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Services configured:"
echo "  - airbridge.service      (main application)"
echo "  - airbridge-web.service  (status web server on port 8080)"
echo ""
echo "Boot sequence:"
echo "  1. Pi boots when USB power connected"
echo "  2. Airbridge checks for pending uploads"
echo "  3. If pending: tries cellular upload (3 min timeout)"
echo "  4. Then: loads USB gadget, appears as drive to host"
echo "  5. Monitors for files, harvests, uploads via cellular"
echo "  6. Survives sudden power loss (read-only root)"
echo ""
echo "Web status available at: http://$(hostname).local:8080"
echo ""
echo "IMPORTANT: Reboot now to activate read-only mode!"
echo "  sudo reboot"
