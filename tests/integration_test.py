#!/usr/bin/env python3
"""
Integration Test: USB Air Bridge Device Connected via USB and SSH
Tests assume the Air Bridge device is physically connected via USB and accessible over SSH.
"""

import os
import sys
import time
import subprocess
import tempfile
import uuid
from datetime import datetime

# Configuration for the connected Air Bridge device
DEVICE_CONFIG = {
    'ssh_host': 'pizerologs.local',
    'ssh_user': 'cedric', 
    'usb_device_path': None,  # Will be auto-detected
    'mount_point': '/tmp/airbridge_test',
    'test_file_size': 1024 * 100,  # 100KB test files
    'quiet_window': 30,  # seconds to wait for file system to stabilize
}

class USBAirBridgeIntegrationTest:
    def __init__(self):
        self.ssh_prefix = f"ssh {DEVICE_CONFIG['ssh_user']}@{DEVICE_CONFIG['ssh_host']}"
        self.usb_device = None
        self.mount_point = DEVICE_CONFIG['mount_point']
        self.test_files = []
        
    def run_local_cmd(self, cmd, check=True, capture=True):
        """Run a command on the local machine."""
        print(f"LOCAL: {cmd}")
        result = subprocess.run(cmd, shell=True, capture_output=capture, text=True)
        if capture and result.stdout:
            print(f"STDOUT: {result.stdout.strip()}")
        if capture and result.stderr:
            print(f"STDERR: {result.stderr.strip()}")
        if check and result.returncode != 0:
            raise Exception(f"Local command failed: {cmd}")
        return result
        
    def run_remote_cmd(self, cmd, check=True, capture=True):
        """Run a command on the remote Air Bridge device via SSH."""
        full_cmd = f"{self.ssh_prefix} \"{cmd}\""
        print(f"REMOTE: {cmd}")
        result = subprocess.run(full_cmd, shell=True, capture_output=capture, text=True)
        if capture and result.stdout:
            print(f"STDOUT: {result.stdout.strip()}")
        if capture and result.stderr:
            print(f"STDERR: {result.stderr.strip()}")
        if check and result.returncode != 0:
            raise Exception(f"Remote command failed: {cmd}")
        return result
        
    def detect_usb_device(self):
        """Detect the Air Bridge USB device on the local machine."""
        print("Detecting USB Air Bridge device...")
        
        # Get list of mass storage devices
        result = self.run_local_cmd("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT | grep disk", check=False)
        devices = []
        
        for line in result.stdout.split('\n'):
            if line.strip() and 'disk' in line:
                parts = line.split()
                if len(parts) >= 3:
                    name = parts[0]
                    size = parts[1]
                    devices.append(f"/dev/{name}")
                    print(f"  Found storage device: /dev/{name} ({size})")
        
        # Look for recently connected devices in dmesg
        result = self.run_local_cmd("dmesg | tail -20 | grep -i usb", check=False)
        if result.stdout:
            print("Recent USB activity:")
            for line in result.stdout.split('\n'):
                if line.strip():
                    print(f"  {line.strip()}")
        
        # For now, we'll need manual selection or auto-detect based on size/timing
        # This is a simplified approach - in practice you might want more sophisticated detection
        if devices:
            # Use the last detected device as a simple heuristic
            self.usb_device = devices[-1]
            print(f"Using device: {self.usb_device}")
            return True
        else:
            print("No USB mass storage devices found")
            return False
            
    def test_ssh_connectivity(self):
        """Test SSH connectivity to the Air Bridge device."""
        print("Testing SSH connectivity to Air Bridge device...")
        try:
            result = self.run_remote_cmd("echo 'SSH connection successful' && hostname && uptime")
            return True
        except Exception as e:
            print(f"SSH connectivity test failed: {e}")
            return False
            
    def test_device_status(self):
        """Check the status of the Air Bridge device and services."""
        print("Checking Air Bridge device status...")
        try:
            # Check if airbridge service is running
            self.run_remote_cmd("systemctl is-active airbridge.service")
            print("✓ Airbridge service is active")
            
            # Check USB gadget status
            result = self.run_remote_cmd("cat /sys/class/udc/*/state 2>/dev/null || echo 'not_configured'")
            state = result.stdout.strip()
            print(f"✓ USB gadget state: {state}")
            
            # Check for virtual disk
            self.run_remote_cmd("ls -la /dev/mmcblk0p3 || ls -la /piusb.bin")
            print("✓ Virtual disk found")
            
            return True
        except Exception as e:
            print(f"Device status check failed: {e}")
            return False
            
    def create_test_files(self):
        """Create test files on the local USB device."""
        print("Creating test files on USB device...")
        
        # Ensure mount point exists
        os.makedirs(self.mount_point, exist_ok=True)
        
        # Mount the USB device locally
        self.run_local_cmd(f"sudo umount {self.mount_point} 2>/dev/null || true", check=False)
        self.run_local_cmd(f"sudo mount {self.usb_device} {self.mount_point}")
        
        # Create test files
        for i in range(3):
            filename = f"test_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:8]}.txt"
            filepath = os.path.join(self.mount_point, filename)
            
            # Generate test content
            test_data = f"""Air Bridge Integration Test Log
============================================
File: {filename}
Timestamp: {datetime.now().isoformat()}
Test ID: {uuid.uuid4()}
Size: {DEVICE_CONFIG['test_file_size']} bytes

{'=' * 50}
Test Data:
{'X' * (DEVICE_CONFIG['test_file_size'] - 200)}
"""
            
            with open(filepath, 'w') as f:
                f.write(test_data)
                
            self.test_files.append(filename)
            print(f"  Created: {filename} ({len(test_data)} bytes)")
            
        # Sync and unmount
        self.run_local_cmd("sync")
        time.sleep(1)
        self.run_local_cmd(f"sudo umount {self.mount_point}")
        
        print(f"Created {len(self.test_files)} test files")
        return True
        
    def wait_for_file_harvesting(self):
        """Wait for the Air Bridge to detect and harvest the files."""
        print(f"Waiting for file harvesting (timeout: {DEVICE_CONFIG['quiet_window'] + 60}s)...")
        
        start_time = time.time()
        timeout = DEVICE_CONFIG['quiet_window'] + 60
        
        while time.time() - start_time < timeout:
            try:
                # Check if files were harvested by looking at outbox
                result = self.run_remote_cmd("ls -la /home/cedric/outbox/*.zip 2>/dev/null | wc -l", check=False)
                archive_count = int(result.stdout.strip())
                
                if archive_count > 0:
                    print(f"✓ Found {archive_count} archive(s) in outbox")
                    return True
                    
                print(f"  Waiting... ({int(time.time() - start_time)}s)", end='\r')
                time.sleep(5)
                
            except Exception as e:
                print(f"Error checking harvest status: {e}")
                
        print(f"\nTimeout waiting for file harvesting")
        return False
        
    def verify_file_upload(self):
        """Verify that files were uploaded via cellular connection."""
        print("Verifying file upload...")
        
        try:
            # Check modem connectivity
            result = self.run_remote_cmd("python3 -c \"from airbridge.modem_handler import SIM7000GHandler; m=SIM7000GHandler('/dev/ttyAMA0',115200,1); print('Signal:', m.check_signal())\"")
            print("✓ Cellular modem responsive")
            
            # Check if outbox is empty (files uploaded and deleted)
            result = self.run_remote_cmd("ls /home/cedric/outbox/ 2>/dev/null | wc -l")
            remaining_files = int(result.stdout.strip())
            
            if remaining_files == 0:
                print("✓ Outbox is empty - files likely uploaded successfully")
                return True
            else:
                print(f"⚠ {remaining_files} files remain in outbox - upload may have failed")
                
                # Show what's left
                self.run_remote_cmd("ls -la /home/cedric/outbox/")
                return False
                
        except Exception as e:
            print(f"Upload verification failed: {e}")
            return False
            
    def check_device_logs(self):
        """Check Air Bridge device logs for any issues."""
        print("Checking device logs...")
        
        try:
            # Check systemd journal for airbridge service
            print("\n--- Airbridge Service Logs ---")
            self.run_remote_cmd("journalctl -u airbridge.service --no-pager -n 20")
            
            # Check for any Python errors
            print("\n--- Recent Python Errors ---")
            self.run_remote_cmd("journalctl --no-pager -n 50 | grep -i python || echo 'No Python errors found'")
            
            return True
        except Exception as e:
            print(f"Log check failed: {e}")
            return False
            
    def cleanup(self):
        """Clean up test files and reset device state."""
        print("Cleaning up...")
        
        try:
            # Clean up local mount point
            self.run_local_cmd(f"sudo umount {self.mount_point} 2>/dev/null || true", check=False)
            if os.path.exists(self.mount_point):
                os.rmdir(self.mount_point)
                
            # Clean up remote outbox
            self.run_remote_cmd("rm -f /home/cedric/outbox/test_log_*.zip")
            
            # Restart airbridge service to reset state
            self.run_remote_cmd("sudo systemctl restart airbridge.service")
            
            print("✓ Cleanup completed")
        except Exception as e:
            print(f"Cleanup failed: {e}")
            
    def run_full_test(self):
        """Run the complete integration test suite."""
        print("="*60)
        print("USB AIR BRIDGE INTEGRATION TEST")
        print("="*60)
        print(f"Started at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print(f"Target device: {DEVICE_CONFIG['ssh_user']}@{DEVICE_CONFIG['ssh_host']}")
        
        test_results = {}
        
        try:
            # Test 1: SSH Connectivity
            print(f"\n{'='*20} Test 1: SSH Connectivity {'='*20}")
            test_results['ssh'] = self.test_ssh_connectivity()
            
            if not test_results['ssh']:
                print("ABORTING: Cannot connect to device via SSH")
                return test_results
                
            # Test 2: Device Status
            print(f"\n{'='*20} Test 2: Device Status {'='*20}")
            test_results['status'] = self.test_device_status()
            
            # Test 3: USB Device Detection
            print(f"\n{'='*20} Test 3: USB Device Detection {'='*20}")
            test_results['usb_detect'] = self.detect_usb_device()
            
            if not test_results['usb_detect']:
                print("ABORTING: Cannot detect USB Air Bridge device")
                return test_results
                
            # Test 4: File Transfer
            print(f"\n{'='*20} Test 4: File Transfer {'='*20}")
            test_results['file_transfer'] = self.create_test_files()
            
            # Test 5: File Harvesting
            print(f"\n{'='*20} Test 5: File Harvesting {'='*20}")
            test_results['harvesting'] = self.wait_for_file_harvesting()
            
            # Test 6: Upload Verification  
            print(f"\n{'='*20} Test 6: Upload Verification {'='*20}")
            test_results['upload'] = self.verify_file_upload()
            
            # Test 7: Log Analysis
            print(f"\n{'='*20} Test 7: Log Analysis {'='*20}")
            test_results['logs'] = self.check_device_logs()
            
        except KeyboardInterrupt:
            print("\n\nTest interrupted by user")
        except Exception as e:
            print(f"\nTest failed with exception: {e}")
        finally:
            self.cleanup()
            
        # Results Summary
        print(f"\n{'='*60}")
        print("TEST RESULTS SUMMARY")
        print(f"{'='*60}")
        
        for test_name, result in test_results.items():
            status = "PASS" if result else "FAIL"
            print(f"{test_name:20} {status}")
            
        passed = sum(1 for r in test_results.values() if r)
        total = len(test_results)
        
        print(f"\nOVERALL: {passed}/{total} tests passed")
        print(f"Finished at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        
        return test_results

def main():
    """Main test entry point."""
    if len(sys.argv) > 1 and sys.argv[1] == '--help':
        print(__doc__)
        print("\nUsage:")
        print("  python3 integration_test.py          # Run full test suite")
        print("  python3 integration_test.py --help   # Show this help")
        return 0
        
    test = USBAirBridgeIntegrationTest()
    results = test.run_full_test()
    
    # Return appropriate exit code
    all_passed = all(results.values()) if results else False
    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())