#!/bin/bash
# Deployment script for USB Cellular Airbridge
# Copies files to Raspberry Pi and sets up services

set -e

PI_HOST="cedric@pizerologs.local"
PI_HOME="/home/cedric"

echo "=== USB Cellular Airbridge Deployment ==="

# Check if we can reach the Pi
echo "Testing connection to Pi..."
if ! ping -c 1 pizerologs.local >/dev/null 2>&1; then
    echo "ERROR: Cannot reach pizerologs.local"
    echo "Please check network connection or update PI_HOST in this script"
    exit 1
fi

echo "✓ Pi is reachable"

# Copy Python scripts
echo "Copying Python scripts..."
scp *.py "$PI_HOST:$PI_HOME/"

# Copy configuration
echo "Copying configuration files..."
scp config.yaml "$PI_HOST:$PI_HOME/"
scp setup_otg.sh "$PI_HOST:$PI_HOME/"

echo "✓ Files copied successfully"

# Run OTG setup on Pi
echo "Setting up USB OTG on Pi..."
ssh "$PI_HOST" "sudo bash $PI_HOME/setup_otg.sh"

echo "✓ USB OTG configured"

# Make Python scripts executable
echo "Making scripts executable..."
ssh "$PI_HOST" "chmod +x $PI_HOME/*.py"

echo "✓ Scripts made executable"

# Create systemd service for the USB manager
echo "Setting up systemd service..."
cat << 'EOF' > /tmp/usb-airbridge.service
[Unit]
Description=USB Cellular Airbridge - USB Gadget Manager
After=network.target

[Service]
Type=simple
User=cedric
WorkingDirectory=/home/cedric
ExecStart=/usr/bin/python3 /home/cedric/pi_usb_manager.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

scp /tmp/usb-airbridge.service "$PI_HOST:$PI_HOME/"
ssh "$PI_HOST" "sudo mv $PI_HOME/usb-airbridge.service /etc/systemd/system/"
ssh "$PI_HOST" "sudo systemctl daemon-reload"
ssh "$PI_HOST" "sudo systemctl enable usb-airbridge"

echo "✓ USB manager service created and enabled"

# Create systemd service for the FTP uploader
cat << 'EOF' > /tmp/ftp-uploader.service
[Unit]
Description=USB Cellular Airbridge - FTP Uploader
After=network.target

[Service]
Type=simple
User=cedric
WorkingDirectory=/home/cedric
ExecStart=/usr/bin/python3 /home/cedric/ftp_uploader.py --config /home/cedric/config.yaml
Restart=always
RestartSec=30

[Install]
WantedBy=multi-user.target
EOF

scp /tmp/ftp-uploader.service "$PI_HOST:$PI_HOME/"
ssh "$PI_HOST" "sudo mv $PI_HOME/ftp-uploader.service /etc/systemd/system/"
ssh "$PI_HOST" "sudo systemctl daemon-reload"
ssh "$PI_HOST" "sudo systemctl enable ftp-uploader"

echo "✓ FTP uploader service created and enabled"

# Install Python dependencies
echo "Installing Python dependencies..."
ssh "$PI_HOST" "sudo apt-get update && sudo apt-get install -y python3-serial python3-yaml"

echo "✓ Dependencies installed"

# Create required directories
echo "Creating required directories..."
ssh "$PI_HOST" "sudo mkdir -p /mnt/usb_logs /home/cedric/outbox"
ssh "$PI_HOST" "sudo chown cedric:cedric /mnt/usb_logs /home/cedric/outbox"

echo "✓ Directories created"

# Clean up temporary files
rm -f /tmp/usb-airbridge.service /tmp/ftp-uploader.service

echo ""
echo "=== Deployment Complete ==="
echo ""
echo "Next steps:"
echo "1. Start the services:"
echo "   ssh $PI_HOST 'sudo systemctl start usb-airbridge'"
echo "   ssh $PI_HOST 'sudo systemctl start ftp-uploader'"
echo ""
echo "2. Check service status:"
echo "   ssh $PI_HOST 'sudo systemctl status usb-airbridge'"
echo "   ssh $PI_HOST 'sudo systemctl status ftp-uploader'"
echo ""
echo "3. Monitor logs:"
echo "   ssh $PI_HOST 'tail -f /home/cedric/pi_usb_manager.log'"
echo "   ssh $PI_HOST 'tail -f /home/cedric/ftp_uploader.log'"
echo ""
echo "4. Test USB gadget functionality by plugging Pi into legacy host"
echo ""
echo "Configuration file: /home/cedric/config.yaml"
echo "Update FTP settings in config.yaml as needed"