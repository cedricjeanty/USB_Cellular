#!/usr/bin/env python3
"""
Monitor for Raspberry Pi Zero connection and provide power control options.
"""

import subprocess
import time
import re
import sys
from pathlib import Path

def run_command(cmd):
    """Run a shell command and return the output."""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.stdout, result.stderr, result.returncode
    except Exception as e:
        return "", str(e), 1

def get_usb_devices():
    """Get current USB devices with their bus and device numbers."""
    stdout, stderr, code = run_command("lsusb")
    devices = []
    for line in stdout.strip().split('\n'):
        if line:
            # Parse: Bus 001 Device 002: ID 13d3:56ba IMC Networks Integrated Camera
            match = re.match(r'Bus (\d+) Device (\d+): ID ([0-9a-f:]+) (.+)', line)
            if match:
                bus, device, usb_id, description = match.groups()
                devices.append({
                    'bus': bus,
                    'device': device, 
                    'id': usb_id,
                    'description': description,
                    'full_line': line
                })
    return devices

def get_block_devices():
    """Get current block devices."""
    stdout, stderr, code = run_command("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT")
    return stdout

def find_pi_zero():
    """Look for devices that might be a Pi Zero."""
    devices = get_usb_devices()
    pi_candidates = []
    
    for device in devices:
        desc = device['description'].lower()
        # Pi Zero often appears as mass storage or with Raspberry Pi in name
        if any(keyword in desc for keyword in ['raspberry', 'pi', 'mass storage', 'gadget']):
            pi_candidates.append(device)
    
    return pi_candidates

def get_usb_port_path(bus, device):
    """Get the physical port path for a USB device."""
    # Look in sysfs for the device path
    sysfs_pattern = f"/sys/bus/usb/devices/{bus}-*"
    stdout, stderr, code = run_command(f"ls -d {sysfs_pattern} 2>/dev/null || true")
    
    paths = []
    for line in stdout.strip().split('\n'):
        if line and f"{bus}-" in line:
            paths.append(line)
    
    return paths

def check_uhubctl_available():
    """Check if uhubctl is available for port power control."""
    stdout, stderr, code = run_command("which uhubctl")
    return code == 0

def show_port_control_options(bus, device):
    """Show available port control options."""
    print(f"\nPort control options for Bus {bus} Device {device}:")
    
    if check_uhubctl_available():
        print(f"  uhubctl -l {bus} -a off    # Turn off port")
        print(f"  uhubctl -l {bus} -a on     # Turn on port") 
        print(f"  uhubctl -l {bus} -a cycle  # Power cycle port")
    else:
        print("  uhubctl not available - install with: sudo apt install uhubctl")
        
    # Alternative methods
    print(f"\nAlternative methods:")
    print(f"  echo 0 | sudo tee /sys/bus/usb/devices/{bus}-*/authorized  # Disable")
    print(f"  echo 1 | sudo tee /sys/bus/usb/devices/{bus}-*/authorized  # Enable")

def monitor_for_pi():
    """Continuously monitor for Pi Zero connection."""
    print("Monitoring for Raspberry Pi Zero connection...")
    print("Press Ctrl+C to stop\n")
    
    seen_devices = set()
    
    try:
        while True:
            current_devices = get_usb_devices()
            current_set = set((d['bus'], d['device'], d['id']) for d in current_devices)
            
            # Check for new devices
            new_devices = current_set - seen_devices
            for bus, device, usb_id in new_devices:
                device_info = next(d for d in current_devices if d['bus'] == bus and d['device'] == device)
                print(f"NEW: {device_info['full_line']}")
                
                # Check if this might be our Pi
                if any(keyword in device_info['description'].lower() 
                      for keyword in ['raspberry', 'pi', 'mass storage', 'gadget', 'linux']):
                    print(f"*** POTENTIAL PI ZERO DETECTED! ***")
                    show_port_control_options(bus, device)
                    print()
            
            # Check for removed devices  
            removed_devices = seen_devices - current_set
            for bus, device, usb_id in removed_devices:
                print(f"REMOVED: Bus {bus} Device {device} ID {usb_id}")
            
            seen_devices = current_set
            time.sleep(2)
            
    except KeyboardInterrupt:
        print("\nStopped monitoring.")

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--current":
        print("Current USB devices:")
        devices = get_usb_devices()
        for device in devices:
            print(f"  {device['full_line']}")
        
        print("\nPotential Pi Zero candidates:")
        candidates = find_pi_zero()
        if candidates:
            for candidate in candidates:
                print(f"  {candidate['full_line']}")
                show_port_control_options(candidate['bus'], candidate['device'])
        else:
            print("  None found")
        
        print(f"\nBlock devices:")
        print(get_block_devices())
    else:
        monitor_for_pi()

if __name__ == "__main__":
    main()