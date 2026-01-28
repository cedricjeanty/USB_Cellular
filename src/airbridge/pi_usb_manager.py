#!/usr/bin/env python3
"""
Raspberry Pi USB Gadget Manager
Handles the duty cycle of USB gadget mode and file harvesting.
"""

import os
import time
import subprocess
import logging
import shutil
import zipfile
from pathlib import Path

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('/home/cedric/pi_usb_manager.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# Configuration
USB_IMAGE_PATH = "/dev/mmcblk0p3"  # Use DATA partition
MOUNT_POINT = "/mnt/usb_logs"
OUTBOX_DIR = "/home/cedric/outbox"
QUIET_WINDOW_SECONDS = 30

class USBGadgetManager:
    def __init__(self):
        self.gadget_loaded = False
        self.last_mtime = 0
        
        # Ensure directories exist
        os.makedirs(MOUNT_POINT, exist_ok=True)
        os.makedirs(OUTBOX_DIR, exist_ok=True)
    
    def load_gadget(self):
        """Load the USB mass storage gadget."""
        try:
            cmd = [
                'sudo', 'modprobe', 'g_mass_storage',
                f'file={USB_IMAGE_PATH}',
                'stall=0',
                'removable=1'
            ]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode == 0:
                self.gadget_loaded = True
                logger.info("USB gadget loaded successfully")
                return True
            else:
                logger.error(f"Failed to load gadget: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error loading gadget: {e}")
            return False
    
    def unload_gadget(self):
        """Unload the USB mass storage gadget."""
        try:
            result = subprocess.run(['sudo', 'modprobe', '-r', 'g_mass_storage'], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                self.gadget_loaded = False
                logger.info("USB gadget unloaded successfully")
                return True
            else:
                logger.error(f"Failed to unload gadget: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error unloading gadget: {e}")
            return False
    
    def get_udc_state(self):
        """Get the UDC state to check if host is connected."""
        try:
            udc_dirs = list(Path('/sys/class/udc').glob('*'))
            if not udc_dirs:
                return None
            
            state_file = udc_dirs[0] / 'state'
            if state_file.exists():
                return state_file.read_text().strip()
            return None
        except Exception as e:
            logger.error(f"Error reading UDC state: {e}")
            return None
    
    def get_image_mtime(self):
        """Get modification time of the USB image."""
        try:
            return os.path.getmtime(USB_IMAGE_PATH)
        except Exception as e:
            logger.error(f"Error getting image mtime: {e}")
            return 0
    
    def wait_for_quiet_window(self):
        """Wait for the quiet window (no writes for specified duration)."""
        logger.info(f"Waiting for {QUIET_WINDOW_SECONDS}s quiet window...")
        
        start_time = time.time()
        last_activity = start_time
        
        while True:
            current_mtime = self.get_image_mtime()
            current_time = time.time()
            
            # Check if image was modified
            if current_mtime > self.last_mtime:
                self.last_mtime = current_mtime
                last_activity = current_time
                logger.info("File activity detected, resetting quiet window timer")
            
            # Check if we have a quiet window
            quiet_duration = current_time - last_activity
            if quiet_duration >= QUIET_WINDOW_SECONDS:
                logger.info("Quiet window achieved")
                return True
            
            # Check UDC state - if not configured, host disconnected
            state = self.get_udc_state()
            if state != 'configured':
                logger.info(f"UDC state changed to '{state}', host disconnected")
                return True
            
            time.sleep(2)
    
    def mount_image(self):
        """Mount the USB image locally."""
        try:
            # Ensure mount point exists and is empty
            if os.path.ismount(MOUNT_POINT):
                subprocess.run(['sudo', 'umount', MOUNT_POINT], check=False)
            
            result = subprocess.run([
                'sudo', 'mount', '-o', 'loop', USB_IMAGE_PATH, MOUNT_POINT
            ], capture_output=True, text=True)
            
            if result.returncode == 0:
                logger.info(f"Image mounted at {MOUNT_POINT}")
                return True
            else:
                logger.error(f"Failed to mount image: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error mounting image: {e}")
            return False
    
    def unmount_image(self):
        """Unmount the USB image."""
        try:
            result = subprocess.run(['sudo', 'umount', MOUNT_POINT], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                logger.info("Image unmounted successfully")
                return True
            else:
                logger.error(f"Failed to unmount image: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error unmounting image: {e}")
            return False
    
    def harvest_files(self):
        """Harvest new files from the mounted image."""
        try:
            if not os.path.ismount(MOUNT_POINT):
                logger.error("Mount point is not mounted")
                return False
            
            # Create timestamped archive
            timestamp = int(time.time())
            archive_name = f"harvest_{timestamp}.zip"
            archive_path = os.path.join(OUTBOX_DIR, archive_name)
            
            files_found = False
            
            with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
                for root, dirs, files in os.walk(MOUNT_POINT):
                    for file in files:
                        file_path = os.path.join(root, file)
                        arcname = os.path.relpath(file_path, MOUNT_POINT)
                        zipf.write(file_path, arcname)
                        files_found = True
                        logger.info(f"Added to archive: {arcname}")
            
            if files_found:
                logger.info(f"Created archive: {archive_path}")
                
                # Sync to ensure data is written
                os.sync()
                return True
            else:
                # Remove empty archive
                os.remove(archive_path)
                logger.info("No files found to harvest")
                return True
                
        except Exception as e:
            logger.error(f"Error harvesting files: {e}")
            return False
    
    def run_duty_cycle(self):
        """Run one complete duty cycle."""
        logger.info("Starting duty cycle...")
        
        try:
            # Phase 1: Load gadget and wait for activity
            if not self.load_gadget():
                return False
            
            # Wait for quiet window
            self.wait_for_quiet_window()
            
            # Phase 2: Harvest files
            if not self.unload_gadget():
                return False
            
            # Small delay to ensure gadget is fully unloaded
            time.sleep(2)
            
            if not self.mount_image():
                return False
            
            self.harvest_files()
            
            if not self.unmount_image():
                return False
            
            logger.info("Duty cycle completed successfully")
            return True
            
        except Exception as e:
            logger.error(f"Error in duty cycle: {e}")
            return False
    
    def cleanup(self):
        """Cleanup - ensure gadget is unloaded and image is unmounted."""
        if self.gadget_loaded:
            self.unload_gadget()
        
        if os.path.ismount(MOUNT_POINT):
            self.unmount_image()

def main():
    """Main loop."""
    manager = USBGadgetManager()
    
    try:
        while True:
            success = manager.run_duty_cycle()
            if not success:
                logger.error("Duty cycle failed, waiting before retry...")
                time.sleep(30)
            else:
                logger.info("Duty cycle completed, waiting for next cycle...")
                time.sleep(10)
                
    except KeyboardInterrupt:
        logger.info("Received interrupt, cleaning up...")
    except Exception as e:
        logger.error(f"Unexpected error: {e}")
    finally:
        manager.cleanup()

if __name__ == "__main__":
    main()