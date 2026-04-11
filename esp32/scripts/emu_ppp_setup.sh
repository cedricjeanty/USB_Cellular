#!/bin/bash
# Setup IP forwarding and NAT for the emulator's PPP tunnel.
# Run once before starting the emulator (requires sudo).
#
# The SimModem's pppd server assigns 10.64.64.2 to the client.
# This script enables forwarding so traffic from the PPP tunnel
# reaches the internet via the host's default route.

set -e

echo "Enabling IP forwarding..."
sudo sysctl -w net.ipv4.ip_forward=1

echo "Setting up NAT for PPP subnet..."
sudo iptables -t nat -C POSTROUTING -s 10.64.64.0/24 -j MASQUERADE 2>/dev/null || \
sudo iptables -t nat -A POSTROUTING -s 10.64.64.0/24 -j MASQUERADE

echo "Done. PPP tunnel traffic from 10.64.64.0/24 will be NATted to the internet."
echo "Run the emulator with: cd esp32 && .pio/build/emulator/program [device_id]"
