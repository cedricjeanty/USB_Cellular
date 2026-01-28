#!/usr/bin/env python3
"""
Monitor USB DATA device for file changes and harvest files when quiet.
This script monitors the locally mounted DATA device that appears when 
the Pi's USB gadget is active.
"""

import os
import time
import subprocess
import logging
from pathlib import Path

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('usb_monitor.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

QUIET_WINDOW_SECONDS = 30
DATA_DEVICE_LABEL = "DATA"

def find_data_device():
    """Find the DATA device mount point."""
    try:
        # Check mounted filesystems for DATA label
        result = subprocess.run(['lsblk', '-o', 'NAME,LABEL,MOUNTPOINT'], 
                              capture_output=True, text=True)
        for line in result.stdout.split('\n'):
            if DATA_DEVICE_LABEL in line and '/media' in line:
                parts = line.split()
                for part in parts:
                    if part.startswith('/media') or part.startswith('/mnt'):
                        return part
        return None
    except Exception as e:
        logger.error(f"Error finding DATA device: {e}")
        return None

def get_directory_mtime(path):
    """Get the most recent modification time in a directory tree."""
    try:
        latest_mtime = 0
        for root, dirs, files in os.walk(path):
            # Check directory mtime
            dir_mtime = os.path.getmtime(root)
            latest_mtime = max(latest_mtime, dir_mtime)
            
            # Check file mtimes
            for file in files:
                file_path = os.path.join(root, file)
                if os.path.exists(file_path):
                    file_mtime = os.path.getmtime(file_path)
                    latest_mtime = max(latest_mtime, file_mtime)
        
        return latest_mtime
    except Exception as e:
        logger.error(f"Error getting directory mtime: {e}")
        return 0

def list_new_files(path, since_time):
    """List files modified since the given time."""
    new_files = []
    try:
        for root, dirs, files in os.walk(path):
            for file in files:
                file_path = os.path.join(root, file)
                if os.path.exists(file_path) and os.path.getmtime(file_path) > since_time:
                    new_files.append(file_path)
    except Exception as e:
        logger.error(f"Error listing new files: {e}")
    
    return new_files

def main():
    """Main monitoring loop."""
    logger.info("Starting USB DATA device monitor...")
    
    # Record start time to track new files
    start_time = time.time()
    
    while True:
        # Find the DATA device
        data_mount = find_data_device()
        
        if not data_mount:
            logger.info("DATA device not found, waiting...")
            time.sleep(5)
            continue
            
        logger.info(f"Found DATA device at: {data_mount}")
        
        # Monitor for quiet window
        last_activity_time = time.time()
        
        while True:
            current_mtime = get_directory_mtime(data_mount)
            current_time = time.time()
            
            # If files were modified recently, update last activity time
            if current_mtime > last_activity_time:
                last_activity_time = current_time
                logger.info("File activity detected, resetting quiet window timer")
            
            # Check if we've had a quiet window
            quiet_duration = current_time - last_activity_time
            
            if quiet_duration >= QUIET_WINDOW_SECONDS:
                logger.info(f"Quiet window of {QUIET_WINDOW_SECONDS}s detected")
                
                # List files that were added since we started monitoring
                new_files = list_new_files(data_mount, start_time)
                
                if new_files:
                    logger.info(f"Found {len(new_files)} new files:")
                    for file_path in new_files:
                        rel_path = os.path.relpath(file_path, data_mount)
                        file_size = os.path.getsize(file_path)
                        logger.info(f"  {rel_path} ({file_size} bytes)")
                else:
                    logger.info("No new files detected")
                
                # Signal that the Pi should unmount the gadget
                logger.info("Ready to unmount USB gadget - send signal to Pi")
                break
            
            # Wait a bit before next check
            time.sleep(2)
        
        # Wait for device to disappear (indicating Pi has unmounted)
        logger.info("Waiting for DATA device to be unmounted...")
        while find_data_device():
            time.sleep(1)
        
        logger.info("DATA device unmounted. Cycle complete.")
        
        # Wait a bit before starting next cycle
        time.sleep(5)

if __name__ == "__main__":
    main()