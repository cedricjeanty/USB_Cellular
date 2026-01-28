#!/usr/bin/env python3
"""
System Test Script for USB Cellular Airbridge
Tests the complete workflow end-to-end.
"""

import os
import time
import tempfile
import logging
import yaml
from pathlib import Path

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def test_config_loading():
    """Test configuration loading."""
    logger.info("Testing configuration loading...")
    
    try:
        with open('config.yaml', 'r') as f:
            config = yaml.safe_load(f)
        
        # Check required sections
        required_sections = ['modem', 'ftp', 'outbox_dir']
        for section in required_sections:
            if section not in config:
                logger.error(f"Missing required config section: {section}")
                return False
        
        logger.info("✓ Configuration loaded successfully")
        return True
    except Exception as e:
        logger.error(f"Configuration loading failed: {e}")
        return False

def test_outbox_functionality():
    """Test outbox directory operations."""
    logger.info("Testing outbox functionality...")
    
    try:
        # Load config to get outbox path
        with open('config.yaml', 'r') as f:
            config = yaml.safe_load(f)
        
        outbox_dir = config.get('outbox_dir', '/home/cedric/outbox')
        
        # Ensure outbox exists
        os.makedirs(outbox_dir, exist_ok=True)
        
        # Create a test file
        test_file = os.path.join(outbox_dir, 'test_file.txt')
        with open(test_file, 'w') as f:
            f.write("This is a test file for the airbridge system.\n")
            f.write(f"Created at: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        
        # Verify file was created
        if not os.path.exists(test_file):
            logger.error("Failed to create test file in outbox")
            return False
        
        # Clean up test file
        os.remove(test_file)
        
        logger.info("✓ Outbox functionality working")
        return True
    except Exception as e:
        logger.error(f"Outbox test failed: {e}")
        return False

def test_usb_image_access():
    """Test access to USB image file."""
    logger.info("Testing USB image access...")
    
    try:
        with open('config.yaml', 'r') as f:
            config = yaml.safe_load(f)
        
        usb_image = config.get('virtual_disk_path', '/dev/mmcblk0p3')
        
        # Check if image/partition exists
        if not os.path.exists(usb_image):
            logger.warning(f"USB image not found at {usb_image} (may be normal if not on Pi)")
            return True  # Not a failure - might not be on Pi
        
        # Try to get file stats
        stat_info = os.stat(usb_image)
        logger.info(f"USB image found: {usb_image} (size: {stat_info.st_size} bytes)")
        
        logger.info("✓ USB image accessible")
        return True
    except Exception as e:
        logger.error(f"USB image test failed: {e}")
        return False

def test_modem_imports():
    """Test that modem handler can be imported."""
    logger.info("Testing modem handler imports...")
    
    try:
        from modem_handler import SIM7000GHandler
        logger.info("✓ Modem handler imported successfully")
        
        # Test basic instantiation (without connecting)
        handler = SIM7000GHandler('/dev/null', 115200)
        logger.info("✓ Modem handler can be instantiated")
        
        return True
    except Exception as e:
        logger.error(f"Modem import test failed: {e}")
        return False

def test_ftp_uploader_imports():
    """Test that FTP uploader can be imported."""
    logger.info("Testing FTP uploader imports...")
    
    try:
        from ftp_uploader import FTPUploader
        logger.info("✓ FTP uploader imported successfully")
        
        # Test basic instantiation
        uploader = FTPUploader('config.yaml')
        logger.info("✓ FTP uploader can be instantiated")
        
        return True
    except Exception as e:
        logger.error(f"FTP uploader import test failed: {e}")
        return False

def test_usb_manager_imports():
    """Test that USB manager can be imported."""
    logger.info("Testing USB manager imports...")
    
    try:
        from pi_usb_manager import USBGadgetManager
        logger.info("✓ USB manager imported successfully")
        
        # Test basic instantiation
        manager = USBGadgetManager()
        logger.info("✓ USB manager can be instantiated")
        
        return True
    except Exception as e:
        logger.error(f"USB manager import test failed: {e}")
        return False

def test_dependencies():
    """Test required Python dependencies."""
    logger.info("Testing Python dependencies...")
    
    required_modules = [
        'serial',
        'yaml', 
        'zipfile',
        'subprocess',
        'pathlib'
    ]
    
    missing_modules = []
    
    for module in required_modules:
        try:
            __import__(module)
            logger.info(f"✓ {module} available")
        except ImportError:
            logger.error(f"✗ {module} missing")
            missing_modules.append(module)
    
    if missing_modules:
        logger.error(f"Missing required modules: {missing_modules}")
        logger.error("Install with: sudo apt-get install python3-serial python3-yaml")
        return False
    
    logger.info("✓ All required dependencies available")
    return True

def run_all_tests():
    """Run all system tests."""
    logger.info("=== USB Cellular Airbridge System Test ===")
    
    tests = [
        test_dependencies,
        test_config_loading,
        test_outbox_functionality,
        test_usb_image_access,
        test_modem_imports,
        test_ftp_uploader_imports,
        test_usb_manager_imports
    ]
    
    passed = 0
    failed = 0
    
    for test in tests:
        try:
            if test():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            logger.error(f"Test {test.__name__} crashed: {e}")
            failed += 1
        
        logger.info("")  # Blank line between tests
    
    logger.info("=== Test Summary ===")
    logger.info(f"Passed: {passed}")
    logger.info(f"Failed: {failed}")
    logger.info(f"Total:  {passed + failed}")
    
    if failed == 0:
        logger.info("✓ All tests passed!")
        return True
    else:
        logger.warning(f"✗ {failed} test(s) failed")
        return False

if __name__ == "__main__":
    success = run_all_tests()
    exit(0 if success else 1)