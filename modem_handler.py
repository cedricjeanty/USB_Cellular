import serial
import time
import os
import json

# Progress file for resume capability
PROGRESS_FILE = "/tmp/airbridge_upload_progress.json"

class SIM7000GHandler:
    def __init__(self, port, baudrate, timeout=1, chunk_size=512000):
        """Initializes the serial connection to the SIM7000G HAT."""
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.chunk_size = chunk_size

    def send_at(self, command, expected="OK", timeout=5, verbose=True):
        """Sends an AT command and waits for a specific response."""
        self.ser.reset_input_buffer()
        self.ser.write((command + "\r\n").encode())
        if verbose:
            print(f"> {command}")
        start_time = time.time()
        response = ""
        while time.time() - start_time < timeout:
            if self.ser.in_waiting:
                response += self.ser.read(self.ser.in_waiting).decode(errors='ignore')
                if expected in response or "ERROR" in response:
                    if verbose:
                        print(response.strip())
                    return expected in response, response
            time.sleep(0.05)
        if verbose:
            print(f"Timeout. Got: {response.strip()}")
        return False, response

    def wait_for_response(self, expected, timeout=30):
        """Wait for a specific response from modem."""
        start = time.time()
        response = ""
        while time.time() - start < timeout:
            if self.ser.in_waiting:
                response += self.ser.read(self.ser.in_waiting).decode(errors='ignore')
                if expected in response:
                    print(response.strip())
                    return True, response
                if "ERROR" in response:
                    print(f"Error: {response.strip()}")
                    return False, response
            time.sleep(0.1)
        print(f"Timeout waiting for {expected}. Got: {response}")
        return False, response

    def setup_network(self, apn="hologram"):
        """Configures the APN and brings up the wireless connection."""
        print("Setting up cellular network...")

        # Check modem is responsive
        ok, _ = self.send_at("AT", timeout=2)
        if not ok:
            return False, "Modem not responding"

        # Check signal strength
        self.send_at("AT+CSQ")

        # Full functionality mode
        self.send_at("AT+CFUN=1", timeout=5)

        # Wait for network registration
        for i in range(10):
            ok, resp = self.send_at("AT+CREG?", timeout=2)
            if ",1" in resp or ",5" in resp:
                print("Registered on network")
                break
            print(f"Waiting for network... ({i+1}/10)")
            time.sleep(2)

        # Configure bearer for FTP
        self.send_at('AT+SAPBR=3,1,"Contype","GPRS"')
        self.send_at(f'AT+SAPBR=3,1,"APN","{apn}"')
        self.send_at('AT+SAPBR=1,1', timeout=10)  # Open bearer

        # Check we got an IP
        ok, resp = self.send_at('AT+SAPBR=2,1', timeout=5)
        if "0.0.0.0" in resp:
            return False, "Failed to get IP address"

        print("Network ready")
        return True, resp

    def check_signal(self):
        """Check signal strength. Returns (ok, rssi) where rssi 0-31, 99=unknown."""
        ok, resp = self.send_at("AT+CSQ", timeout=2, verbose=False)
        if ok and "+CSQ:" in resp:
            try:
                rssi = int(resp.split(":")[1].split(",")[0].strip())
                return True, rssi
            except:
                pass
        return False, 99

    def save_progress(self, filepath, bytes_sent):
        """Save upload progress to file for resume capability."""
        try:
            progress = {
                "filepath": filepath,
                "bytes_sent": bytes_sent,
                "timestamp": time.time()
            }
            with open(PROGRESS_FILE, 'w') as f:
                json.dump(progress, f)
        except Exception as e:
            print(f"Warning: Could not save progress: {e}")

    def load_progress(self, filepath):
        """Load previous upload progress if available and recent (< 1 hour)."""
        try:
            if not os.path.exists(PROGRESS_FILE):
                return 0
            with open(PROGRESS_FILE, 'r') as f:
                progress = json.load(f)
            # Check if same file and recent (within 1 hour)
            if (progress.get("filepath") == filepath and
                time.time() - progress.get("timestamp", 0) < 3600):
                return progress.get("bytes_sent", 0)
        except:
            pass
        return 0

    def clear_progress(self):
        """Clear progress file after successful upload."""
        try:
            if os.path.exists(PROGRESS_FILE):
                os.remove(PROGRESS_FILE)
        except:
            pass

    def upload_ftp(self, server, port, username, password, filepath, remote_path="/",
                   max_retries=3, retry_delay=5):
        """
        Upload a file via FTP with retry and resume capability.

        Features:
        - Retries failed chunks up to max_retries times
        - Saves progress for resume after power failure
        - Monitors signal strength during upload
        - Uses FTP APPE (append) mode for resume
        """
        if not os.path.exists(filepath):
            return False, f"File not found: {filepath}"

        filename = os.path.basename(filepath)
        filesize = os.path.getsize(filepath)

        # Check for resume from previous attempt
        resume_from = self.load_progress(filepath)
        if resume_from > 0 and resume_from < filesize:
            print(f"Resuming upload from byte {resume_from}/{filesize}")
            use_append = True
        else:
            resume_from = 0
            use_append = False

        print(f"Uploading {filename} ({filesize} bytes) to {server}...")

        # Configure FTP parameters
        self.send_at('AT+FTPCID=1')
        self.send_at(f'AT+FTPSERV="{server}"')
        self.send_at(f'AT+FTPPORT={port}')
        self.send_at('AT+FTPMODE=1')  # Passive mode
        self.send_at(f'AT+FTPUN="{username}"')
        self.send_at(f'AT+FTPPW="{password}"')
        self.send_at(f'AT+FTPPUTNAME="{filename}"')
        self.send_at(f'AT+FTPPUTPATH="{remote_path}"')
        self.send_at('AT+FTPTYPE="I"')  # Binary mode

        # Use APPE (append) mode for resume, STOR for new upload
        if use_append:
            self.send_at('AT+FTPPUTOPT="APPE"')
        else:
            self.send_at('AT+FTPPUTOPT="STOR"')

        # Open FTP PUT session
        self.ser.reset_input_buffer()
        self.ser.write(b"AT+FTPPUT=1\r\n")
        print("> AT+FTPPUT=1")

        # Wait for +FTPPUT: 1,1,<maxlength> which means ready to receive data
        ok, resp = self.wait_for_response("+FTPPUT: 1,1", timeout=60)
        if not ok:
            return False, f"Failed to open FTP session: {resp}"

        # Extract max chunk size from response
        try:
            max_chunk = int(resp.split(",")[-1].strip())
            max_chunk = min(max_chunk, self.chunk_size)
        except:
            max_chunk = 1000  # Safe default

        print(f"FTP ready, max chunk size: {max_chunk}")

        # Send file in chunks with retry logic
        bytes_sent = resume_from
        consecutive_failures = 0
        last_signal_check = time.time()

        with open(filepath, "rb") as f:
            # Seek to resume position
            if resume_from > 0:
                f.seek(resume_from)

            while bytes_sent < filesize:
                # Periodic signal check (every 30 seconds)
                if time.time() - last_signal_check > 30:
                    ok, rssi = self.check_signal()
                    if rssi < 5 and rssi != 99:
                        print(f"Warning: Low signal (CSQ={rssi})")
                    last_signal_check = time.time()

                chunk = f.read(max_chunk)
                if not chunk:
                    break

                chunk_len = len(chunk)
                chunk_sent = False

                # Retry loop for this chunk
                for attempt in range(max_retries):
                    self.ser.reset_input_buffer()
                    self.ser.write(f"AT+FTPPUT=2,{chunk_len}\r\n".encode())

                    # Wait for +FTPPUT: 2,<len> prompt
                    ok, resp = self.wait_for_response("+FTPPUT: 2,", timeout=15)
                    if not ok:
                        print(f"Chunk request failed (attempt {attempt+1}/{max_retries})")
                        if attempt < max_retries - 1:
                            time.sleep(retry_delay)
                        continue

                    # Send the actual data
                    self.ser.write(chunk)

                    # Wait for OK
                    ok, resp = self.wait_for_response("OK", timeout=15)
                    if ok:
                        chunk_sent = True
                        consecutive_failures = 0
                        break
                    else:
                        print(f"Chunk write failed (attempt {attempt+1}/{max_retries})")
                        if attempt < max_retries - 1:
                            time.sleep(retry_delay)

                if not chunk_sent:
                    consecutive_failures += 1
                    # Save progress before failing
                    self.save_progress(filepath, bytes_sent)
                    return False, f"Failed at byte {bytes_sent} after {max_retries} retries"

                bytes_sent += chunk_len
                pct = int(100 * bytes_sent / filesize)
                print(f"Progress: {bytes_sent}/{filesize} bytes ({pct}%)")

                # Save progress periodically (every ~500KB)
                if bytes_sent % (500 * 1024) < max_chunk:
                    self.save_progress(filepath, bytes_sent)

        # Close the FTP session by sending 0 bytes
        time.sleep(0.5)
        self.send_at("AT+FTPPUT=2,0", timeout=5)

        # Wait for final status +FTPPUT: 1,0 means success
        ok, resp = self.wait_for_response("+FTPPUT: 1,0", timeout=30)
        if ok:
            print(f"Upload complete: {filename}")
            self.clear_progress()  # Clear progress file on success
            return True, "Upload successful"
        else:
            self.save_progress(filepath, bytes_sent)
            return False, f"Upload may have failed: {resp}"

    def close_bearer(self):
        """Close the GPRS bearer."""
        self.send_at('AT+SAPBR=0,1', timeout=5)

    def upload_file(self, url, filepath):
        """Uploads a large file by chunking it into 500KB segments."""
        if not os.path.exists(filepath):
            return False, "File not found"

        filesize = os.path.getsize(filepath)
        filename = os.path.basename(filepath)

        with open(filepath, "rb") as f:
            chunk_index = 0
            while True:
                chunk = f.read(self.chunk_size)
                if not chunk:
                    break
                
                # Start HTTP session for this chunk
                self.send_at("AT+HTTPINIT")
                self.send_at(f'AT+HTTPPARA="URL","{url}"')
                self.send_at('AT+HTTPPARA="CONTENT","application/octet-stream"')
                
                # Tell modem how much data to expect for this chunk
                # Timeout is 10s to allow for slow serial transfer
                self.ser.write(f"AT+HTTPDATA={len(chunk)},10000\r\n".encode())
                time.sleep(0.5) 
                self.ser.write(chunk)
                
                # Execute POST
                success, resp = self.send_at("AT+HTTPACTION=1", expected="+HTTPACTION: 1,200", timeout=30)
                self.send_at("AT+HTTPTERM")
                
                if not success:
                    return False, f"Failed at chunk {chunk_index}: {resp}"
                
                print(f"Successfully uploaded chunk {chunk_index} of {filename}")
                chunk_index += 1
        
        return True, "Upload Complete"