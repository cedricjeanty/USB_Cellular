#!/usr/bin/env python3
"""
SSH Device Test: Focused SSH connectivity and device management tests
Tests SSH connectivity, remote command execution, and device status monitoring.
"""

import os
import sys
import time
import subprocess
import json
from datetime import datetime

# Configuration
SSH_CONFIG = {
    'host': 'pizerologs.local',
    'user': 'cedric',
    'timeout': 10,
    'port': 22
}

class SSHDeviceTest:
    def __init__(self):
        self.ssh_prefix = f"ssh -o ConnectTimeout={SSH_CONFIG['timeout']} {SSH_CONFIG['user']}@{SSH_CONFIG['host']}"
        
    def run_ssh_cmd(self, cmd, check=True, timeout=30):
        """Execute command on remote device via SSH."""
        full_cmd = f"{self.ssh_prefix} \"{cmd}\""
        print(f"SSH: {cmd}")
        
        try:
            result = subprocess.run(
                full_cmd, 
                shell=True, 
                capture_output=True, 
                text=True, 
                timeout=timeout
            )
            
            if result.stdout:
                print(f"STDOUT: {result.stdout.strip()}")
            if result.stderr:
                print(f"STDERR: {result.stderr.strip()}")
                
            if check and result.returncode != 0:
                raise Exception(f"SSH command failed (exit {result.returncode}): {cmd}")
                
            return result
        except subprocess.TimeoutExpired:
            raise Exception(f"SSH command timed out: {cmd}")
            
    def test_basic_connectivity(self):
        """Test basic SSH connectivity and responsiveness."""
        print("Testing basic SSH connectivity...")
        
        try:
            result = self.run_ssh_cmd("echo 'SSH_TEST_SUCCESS'")
            if 'SSH_TEST_SUCCESS' in result.stdout:
                print("✓ SSH connection established successfully")
                return True
            else:
                print("✗ Unexpected SSH response")
                return False
        except Exception as e:
            print(f"✗ SSH connectivity failed: {e}")
            return False
            
    def test_device_info(self):
        """Gather device information via SSH."""
        print("Gathering device information...")
        
        try:
            # Get hostname
            result = self.run_ssh_cmd("hostname")
            hostname = result.stdout.strip()
            print(f"✓ Hostname: {hostname}")
            
            # Get uptime
            result = self.run_ssh_cmd("uptime")
            uptime = result.stdout.strip()
            print(f"✓ Uptime: {uptime}")
            
            # Get OS info
            result = self.run_ssh_cmd("cat /etc/os-release | grep PRETTY_NAME")
            os_info = result.stdout.strip().split('=')[1].strip('"')
            print(f"✓ OS: {os_info}")
            
            # Get Pi model
            result = self.run_ssh_cmd("cat /proc/cpuinfo | grep 'Model\\|Revision' | head -2")
            pi_info = result.stdout.strip()
            print(f"✓ Pi Info:\n{pi_info}")
            
            return True
        except Exception as e:
            print(f"✗ Failed to gather device info: {e}")
            return False
            
    def test_airbridge_service(self):
        """Test Air Bridge service status and control."""
        print("Testing Air Bridge service...")
        
        try:
            # Check service status
            result = self.run_ssh_cmd("systemctl is-active airbridge.service")
            status = result.stdout.strip()
            print(f"✓ Service status: {status}")
            
            # Check service details
            result = self.run_ssh_cmd("systemctl status airbridge.service --no-pager -l")
            print("✓ Service details retrieved")
            
            # Check if service is enabled
            result = self.run_ssh_cmd("systemctl is-enabled airbridge.service")
            enabled = result.stdout.strip()
            print(f"✓ Service enabled: {enabled}")
            
            return status == 'active'
        except Exception as e:
            print(f"✗ Service check failed: {e}")
            return False
            
    def test_hardware_interfaces(self):
        """Test hardware interfaces (USB, UART, etc.)."""
        print("Testing hardware interfaces...")
        
        try:
            # Check USB gadget interface
            result = self.run_ssh_cmd("ls -la /sys/class/udc/")
            print("✓ USB Device Controller found")
            
            # Check UDC state
            result = self.run_ssh_cmd("cat /sys/class/udc/*/state 2>/dev/null || echo 'no_state'")
            state = result.stdout.strip()
            print(f"✓ UDC State: {state}")
            
            # Check UART interface for modem
            result = self.run_ssh_cmd("ls -la /dev/ttyAMA0")
            print("✓ UART interface (/dev/ttyAMA0) exists")
            
            # Check virtual disk
            result = self.run_ssh_cmd("ls -la /dev/mmcblk0p3 2>/dev/null || ls -la /piusb.bin")
            print("✓ Virtual disk found")
            
            # Check mount point
            result = self.run_ssh_cmd("ls -la /mnt/usb_logs/")
            print("✓ Mount point accessible")
            
            return True
        except Exception as e:
            print(f"✗ Hardware interface check failed: {e}")
            return False
            
    def test_filesystem_access(self):
        """Test filesystem access and permissions."""
        print("Testing filesystem access...")
        
        try:
            # Check home directory access
            result = self.run_ssh_cmd("ls -la /home/cedric/")
            print("✓ Home directory accessible")
            
            # Check outbox directory
            result = self.run_ssh_cmd("ls -la /home/cedric/outbox/ 2>/dev/null || mkdir -p /home/cedric/outbox")
            print("✓ Outbox directory accessible")
            
            # Test write permissions
            test_file = f"/home/cedric/ssh_test_{int(time.time())}.txt"
            result = self.run_ssh_cmd(f"echo 'SSH test file' > {test_file}")
            print("✓ Write permissions working")
            
            # Clean up test file
            result = self.run_ssh_cmd(f"rm {test_file}")
            print("✓ File cleanup successful")
            
            # Check disk space
            result = self.run_ssh_cmd("df -h /home/cedric/")
            print(f"✓ Disk space: {result.stdout.strip().split()[-1]} available")
            
            return True
        except Exception as e:
            print(f"✗ Filesystem access test failed: {e}")
            return False
            
    def test_python_environment(self):
        """Test Python environment and dependencies."""
        print("Testing Python environment...")
        
        try:
            # Check Python version
            result = self.run_ssh_cmd("python3 --version")
            python_version = result.stdout.strip()
            print(f"✓ {python_version}")
            
            # Check if airbridge modules can be imported
            result = self.run_ssh_cmd("cd /home/cedric && python3 -c \"import sys; sys.path.insert(0, 'src'); from airbridge import main; print('Airbridge module OK')\"")
            print("✓ Airbridge module imports successfully")
            
            # Check required Python packages
            packages = ['yaml', 'serial', 'subprocess']
            for pkg in packages:
                try:
                    result = self.run_ssh_cmd(f"python3 -c \"import {pkg}; print('{pkg} OK')\"")
                    print(f"✓ {pkg} package available")
                except:
                    print(f"⚠ {pkg} package missing or issue")
            
            return True
        except Exception as e:
            print(f"✗ Python environment test failed: {e}")
            return False
            
    def test_modem_interface(self):
        """Test cellular modem interface."""
        print("Testing cellular modem interface...")
        
        try:
            # Test serial port access
            result = self.run_ssh_cmd("python3 -c \"import serial; s=serial.Serial('/dev/ttyAMA0', 115200, timeout=1); s.close(); print('Serial port OK')\"")
            print("✓ Serial port accessible")
            
            # Test basic AT command (with timeout)
            result = self.run_ssh_cmd("timeout 10s python3 -c \"import serial; s=serial.Serial('/dev/ttyAMA0', 115200, timeout=2); s.write(b'AT\\r\\n'); resp=s.read(20); s.close(); print('AT response:', resp.decode('utf-8', errors='ignore'))\"", timeout=15)
            print("✓ Basic AT command test completed")
            
            return True
        except Exception as e:
            print(f"⚠ Modem interface test failed (may be normal if no SIM): {e}")
            return False
            
    def test_log_collection(self):
        """Collect and analyze device logs."""
        print("Collecting device logs...")
        
        try:
            # Get system journal for airbridge service
            print("\n--- Recent Airbridge Service Logs ---")
            result = self.run_ssh_cmd("journalctl -u airbridge.service --no-pager -n 10")
            
            # Get system load and memory
            print("\n--- System Resources ---")
            result = self.run_ssh_cmd("free -h && echo '---' && cat /proc/loadavg")
            
            # Check for any errors in logs
            print("\n--- Recent System Errors ---")
            result = self.run_ssh_cmd("journalctl --no-pager -p err -n 5")
            
            # Check process status
            print("\n--- Process Status ---")
            result = self.run_ssh_cmd("ps aux | grep -E '(airbridge|python3)' | grep -v grep")
            
            return True
        except Exception as e:
            print(f"✗ Log collection failed: {e}")
            return False
            
    def test_remote_control(self):
        """Test remote control capabilities."""
        print("Testing remote control capabilities...")
        
        try:
            # Test service restart
            print("Testing service restart...")
            result = self.run_ssh_cmd("sudo systemctl restart airbridge.service")
            time.sleep(3)
            
            # Verify service came back up
            result = self.run_ssh_cmd("systemctl is-active airbridge.service")
            if result.stdout.strip() == 'active':
                print("✓ Service restart successful")
            else:
                print("⚠ Service may not have restarted properly")
                
            # Test config file access
            result = self.run_ssh_cmd("cat /home/cedric/config.yaml | head -5")
            print("✓ Configuration file accessible")
            
            return True
        except Exception as e:
            print(f"✗ Remote control test failed: {e}")
            return False
            
    def run_all_tests(self):
        """Run all SSH device tests."""
        print("="*60)
        print("SSH DEVICE TEST SUITE")
        print("="*60)
        print(f"Started at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print(f"Target: {SSH_CONFIG['user']}@{SSH_CONFIG['host']}")
        
        tests = [
            ("Basic Connectivity", self.test_basic_connectivity),
            ("Device Information", self.test_device_info),
            ("Airbridge Service", self.test_airbridge_service),
            ("Hardware Interfaces", self.test_hardware_interfaces),
            ("Filesystem Access", self.test_filesystem_access),
            ("Python Environment", self.test_python_environment),
            ("Modem Interface", self.test_modem_interface),
            ("Log Collection", self.test_log_collection),
            ("Remote Control", self.test_remote_control),
        ]
        
        results = {}
        
        for test_name, test_func in tests:
            print(f"\n{'='*20} {test_name} {'='*20}")
            try:
                results[test_name] = test_func()
            except Exception as e:
                print(f"✗ Test failed with exception: {e}")
                results[test_name] = False
                
        # Results summary
        print(f"\n{'='*60}")
        print("SSH TEST RESULTS SUMMARY")
        print(f"{'='*60}")
        
        passed = 0
        for test_name, result in results.items():
            status = "PASS" if result else "FAIL"
            print(f"{test_name:25} {status}")
            if result:
                passed += 1
                
        total = len(results)
        print(f"\nOVERALL: {passed}/{total} tests passed")
        print(f"Finished at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        
        return results

def main():
    """Main test entry point."""
    if len(sys.argv) > 1 and sys.argv[1] == '--help':
        print(__doc__)
        print("\nUsage:")
        print("  python3 ssh_device_test.py           # Run all SSH tests")
        print("  python3 ssh_device_test.py --help    # Show this help")
        return 0
        
    test = SSHDeviceTest()
    results = test.run_all_tests()
    
    # Return appropriate exit code
    all_passed = all(results.values()) if results else False
    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())