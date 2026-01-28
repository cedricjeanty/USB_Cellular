#!/usr/bin/env python3
"""
FTP Uploader for harvested files using SIM7000G cellular modem.
Processes files from the outbox directory and uploads them via FTP.
"""

import os
import time
import logging
import yaml
from pathlib import Path
from modem_handler import SIM7000GHandler

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('/home/cedric/ftp_uploader.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class FTPUploader:
    def __init__(self, config_file='config.yaml'):
        """Initialize FTP uploader with configuration."""
        self.config = self.load_config(config_file)
        self.modem = None
        self.outbox_dir = self.config.get('outbox_dir', '/home/cedric/outbox')
        
        # Ensure outbox directory exists
        os.makedirs(self.outbox_dir, exist_ok=True)
    
    def load_config(self, config_file):
        """Load configuration from YAML file."""
        try:
            with open(config_file, 'r') as f:
                return yaml.safe_load(f)
        except Exception as e:
            logger.error(f"Failed to load config: {e}")
            # Return default config
            return {
                'modem': {
                    'port': '/dev/ttyAMA0',
                    'baudrate': 115200,
                    'apn': 'hologram'
                },
                'ftp': {
                    'server': 'ftp.example.com',
                    'port': 21,
                    'username': 'user',
                    'password': 'pass',
                    'remote_path': '/uploads'
                },
                'outbox_dir': '/home/cedric/outbox',
                'max_retries': 3,
                'retry_delay': 10
            }
    
    def initialize_modem(self):
        """Initialize the cellular modem connection."""
        try:
            modem_config = self.config.get('modem', {})
            
            self.modem = SIM7000GHandler(
                port=modem_config.get('port', '/dev/ttyAMA0'),
                baudrate=modem_config.get('baudrate', 115200),
                timeout=1,
                chunk_size=512000  # 512KB chunks
            )
            
            logger.info("Initializing cellular modem...")
            
            # Setup network connection
            success, message = self.modem.setup_network(
                apn=modem_config.get('apn', 'hologram')
            )
            
            if success:
                logger.info("Modem initialized successfully")
                return True
            else:
                logger.error(f"Modem initialization failed: {message}")
                return False
                
        except Exception as e:
            logger.error(f"Error initializing modem: {e}")
            return False
    
    def cleanup_modem(self):
        """Cleanup modem connection."""
        if self.modem:
            try:
                self.modem.close_bearer()
                logger.info("Modem connection closed")
            except Exception as e:
                logger.error(f"Error closing modem: {e}")
    
    def get_pending_files(self):
        """Get list of files pending upload from outbox."""
        try:
            files = []
            for file_path in Path(self.outbox_dir).glob('*'):
                if file_path.is_file():
                    files.append(str(file_path))
            
            # Sort by modification time (oldest first)
            files.sort(key=lambda x: os.path.getmtime(x))
            return files
            
        except Exception as e:
            logger.error(f"Error getting pending files: {e}")
            return []
    
    def upload_file(self, file_path):
        """Upload a single file via FTP."""
        if not self.modem:
            logger.error("Modem not initialized")
            return False
        
        try:
            ftp_config = self.config.get('ftp', {})
            
            logger.info(f"Starting upload: {os.path.basename(file_path)}")
            
            success, message = self.modem.upload_ftp(
                server=ftp_config.get('server'),
                port=ftp_config.get('port', 21),
                username=ftp_config.get('username'),
                password=ftp_config.get('password'),
                filepath=file_path,
                remote_path=ftp_config.get('remote_path', '/'),
                max_retries=self.config.get('max_retries', 3),
                retry_delay=self.config.get('retry_delay', 10)
            )
            
            if success:
                logger.info(f"Upload successful: {os.path.basename(file_path)}")
                # Delete file after successful upload
                try:
                    os.remove(file_path)
                    logger.info(f"Deleted local file: {os.path.basename(file_path)}")
                except Exception as e:
                    logger.warning(f"Failed to delete file after upload: {e}")
                return True
            else:
                logger.error(f"Upload failed: {message}")
                return False
                
        except Exception as e:
            logger.error(f"Error uploading file: {e}")
            return False
    
    def check_signal_quality(self):
        """Check cellular signal quality before uploading."""
        if not self.modem:
            return False, 0
        
        try:
            success, rssi = self.modem.check_signal()
            if success:
                if rssi == 99:
                    logger.warning("Signal strength unknown")
                    return False, rssi
                elif rssi < 5:
                    logger.warning(f"Very weak signal (RSSI: {rssi})")
                    return False, rssi
                elif rssi < 10:
                    logger.warning(f"Weak signal (RSSI: {rssi})")
                    return True, rssi
                else:
                    logger.info(f"Good signal strength (RSSI: {rssi})")
                    return True, rssi
            else:
                logger.error("Failed to check signal strength")
                return False, 0
                
        except Exception as e:
            logger.error(f"Error checking signal: {e}")
            return False, 0
    
    def process_outbox(self):
        """Process all files in the outbox directory."""
        logger.info("Processing outbox for file uploads...")
        
        # Check if modem is initialized
        if not self.modem:
            if not self.initialize_modem():
                logger.error("Cannot process outbox - modem initialization failed")
                return False
        
        # Check signal quality
        signal_ok, rssi = self.check_signal_quality()
        if not signal_ok:
            logger.warning(f"Poor signal quality (RSSI: {rssi}), delaying uploads")
            return False
        
        # Get pending files
        pending_files = self.get_pending_files()
        
        if not pending_files:
            logger.info("No files pending upload")
            return True
        
        logger.info(f"Found {len(pending_files)} files to upload")
        
        # Upload files one by one
        successful_uploads = 0
        failed_uploads = 0
        
        for file_path in pending_files:
            file_size = os.path.getsize(file_path)
            logger.info(f"Uploading {os.path.basename(file_path)} ({file_size} bytes)")
            
            if self.upload_file(file_path):
                successful_uploads += 1
            else:
                failed_uploads += 1
                # Stop on first failure to avoid wasting data/battery
                logger.warning("Stopping uploads due to failure")
                break
        
        logger.info(f"Upload session complete: {successful_uploads} successful, {failed_uploads} failed")
        return failed_uploads == 0
    
    def run_continuous(self, check_interval=300):
        """Run continuous monitoring and upload of outbox files."""
        logger.info(f"Starting continuous FTP uploader (check interval: {check_interval}s)")
        
        try:
            while True:
                try:
                    self.process_outbox()
                except Exception as e:
                    logger.error(f"Error processing outbox: {e}")
                
                # Wait before next check
                logger.info(f"Waiting {check_interval} seconds before next check...")
                time.sleep(check_interval)
                
        except KeyboardInterrupt:
            logger.info("Received interrupt signal, shutting down...")
        except Exception as e:
            logger.error(f"Unexpected error in continuous mode: {e}")
        finally:
            self.cleanup_modem()

def main():
    """Main entry point."""
    import argparse
    
    parser = argparse.ArgumentParser(description='FTP Uploader for USB Cellular Airbridge')
    parser.add_argument('--config', default='config.yaml', help='Configuration file path')
    parser.add_argument('--once', action='store_true', help='Run once and exit')
    parser.add_argument('--interval', type=int, default=300, help='Check interval in seconds')
    
    args = parser.parse_args()
    
    uploader = FTPUploader(args.config)
    
    if args.once:
        # Run once and exit
        logger.info("Running single upload cycle...")
        success = uploader.process_outbox()
        uploader.cleanup_modem()
        exit(0 if success else 1)
    else:
        # Run continuously
        uploader.run_continuous(args.interval)

if __name__ == "__main__":
    main()