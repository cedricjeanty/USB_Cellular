#!/bin/bash
# Setup script for Pi Zero USB OTG Mass Storage
# Run this once on a fresh Pi Zero to configure it as a USB drive

set -e

echo "=== Pi Zero OTG Mass Storage Setup ==="

# Configuration
# Use DATA partition if it exists, otherwise fall back to file-backed image
DATA_PARTITION="/dev/mmcblk0p3"
DISK_IMAGE_FILE="/piusb.bin"
DISK_SIZE_MB=512

# Check for DATA partition
if [ -b "$DATA_PARTITION" ]; then
    DISK_IMAGE="$DATA_PARTITION"
    echo "Found DATA partition: $DATA_PARTITION"
else
    DISK_IMAGE="$DISK_IMAGE_FILE"
    echo "No DATA partition found, will use file-backed image: $DISK_IMAGE"
fi
CONFIG_FILE="/boot/firmware/config.txt"
MODULES_FILE="/etc/modules"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

echo ""
echo "Step 1: Configuring boot config for OTG peripheral mode..."

# Backup config
cp "$CONFIG_FILE" "${CONFIG_FILE}.backup"

# Remove any existing dwc2/otg settings that might conflict
sed -i '/^otg_mode=/d' "$CONFIG_FILE"
sed -i '/^dtoverlay=dwc2/d' "$CONFIG_FILE"

# Check if [all] section exists
if grep -q '^\[all\]' "$CONFIG_FILE"; then
    # Add after [all] section
    sed -i '/^\[all\]/a dtoverlay=dwc2,dr_mode=peripheral' "$CONFIG_FILE"
else
    # Append to end
    echo "" >> "$CONFIG_FILE"
    echo "[all]" >> "$CONFIG_FILE"
    echo "dtoverlay=dwc2,dr_mode=peripheral" >> "$CONFIG_FILE"
fi

echo "  - Added dtoverlay=dwc2,dr_mode=peripheral to [all] section"

echo ""
echo "Step 2: Adding dwc2 to kernel modules..."

if ! grep -q '^dwc2$' "$MODULES_FILE"; then
    echo "dwc2" >> "$MODULES_FILE"
    echo "  - Added dwc2 to $MODULES_FILE"
else
    echo "  - dwc2 already in $MODULES_FILE"
fi

echo ""
echo "Step 3: Setting up virtual disk..."

if [ -b "$DISK_IMAGE" ]; then
    # Using a block device/partition
    echo "  - Using partition: $DISK_IMAGE"
    PART_SIZE=$(lsblk -b -n -o SIZE "$DISK_IMAGE" 2>/dev/null | head -1)
    PART_SIZE_MB=$((PART_SIZE / 1024 / 1024))
    echo "  - Partition size: ${PART_SIZE_MB}MB"

    # Check if formatted
    FSTYPE=$(lsblk -n -o FSTYPE "$DISK_IMAGE" 2>/dev/null | head -1)
    if [ -z "$FSTYPE" ]; then
        echo "  - WARNING: Partition not formatted"
        read -p "  - Format as FAT32? (y/N): " format_it
        if [ "$format_it" = "y" ] || [ "$format_it" = "Y" ]; then
            mkfs.vfat -F 32 "$DISK_IMAGE"
            echo "  - Formatted as FAT32"
        fi
    else
        echo "  - Filesystem: $FSTYPE"
    fi
else
    # Using a file-backed image
    if [ -f "$DISK_IMAGE" ]; then
        echo "  - Disk image $DISK_IMAGE already exists ($(du -h $DISK_IMAGE | cut -f1))"
        read -p "  - Recreate it? (y/N): " recreate
        if [ "$recreate" = "y" ] || [ "$recreate" = "Y" ]; then
            rm "$DISK_IMAGE"
        fi
    fi

    if [ ! -f "$DISK_IMAGE" ]; then
        echo "  - Creating ${DISK_SIZE_MB}MB disk image..."
        dd if=/dev/zero of="$DISK_IMAGE" bs=1M count="$DISK_SIZE_MB" status=progress
        echo "  - Formatting as FAT32..."
        mkfs.vfat "$DISK_IMAGE"
        echo "  - Disk image created"
    fi
fi

echo ""
echo "Step 4: Creating mount point..."

MOUNT_POINT="/mnt/usb_logs"
if [ ! -d "$MOUNT_POINT" ]; then
    mkdir -p "$MOUNT_POINT"
    echo "  - Created $MOUNT_POINT"
else
    echo "  - $MOUNT_POINT already exists"
fi

echo ""
echo "Step 5: Loading modules (for immediate use without reboot)..."

modprobe loop
modprobe dwc2

# Check if UDC is available
sleep 1
if [ -d /sys/class/udc ] && [ "$(ls -A /sys/class/udc)" ]; then
    echo "  - UDC device found: $(ls /sys/class/udc)"

    # Load mass storage gadget
    modprobe -r g_mass_storage 2>/dev/null || true
    modprobe g_mass_storage file="$DISK_IMAGE" stall=0 removable=1
    echo "  - Mass storage gadget loaded"

    UDC_STATE=$(cat /sys/class/udc/*/state 2>/dev/null || echo "unknown")
    echo "  - UDC state: $UDC_STATE"
else
    echo "  - WARNING: No UDC device found. Reboot required."
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "If UDC was not found, please reboot: sudo reboot"
echo ""
echo "After reboot, load the USB gadget with:"
echo "  sudo modprobe g_mass_storage file=$DISK_IMAGE stall=0 removable=1"
echo ""
echo "Or run the usb_test.py script:"
echo "  sudo python3 ~/usb_test.py"
