import serial
import time
import yaml
from datetime import datetime

# Load configuration from shared config file
with open("config.yaml", "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

# Use the UART port connected to the HAT
ser = serial.Serial(
    cfg['serial']['port'],
    cfg['serial']['baudrate'],
    timeout=cfg['serial']['timeout']
)

def send_at(command, wait_time=2):
    """Send AT command and return response."""
    ser.reset_input_buffer()
    ser.write((command + "\r\n").encode())
    time.sleep(wait_time)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
    print(f"> {command}")
    print(response)
    return response

def wait_for_response(expected, timeout=30):
    """Wait for a specific response from modem."""
    start = time.time()
    response = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            if expected in response:
                print(response)
                return True, response
        time.sleep(0.1)
    print(f"Timeout waiting for {expected}. Got: {response}")
    return False, response

def test_ftp_upload():
    # Create test content with timestamp
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"pitest_{timestamp}.txt"
    content = f"Test upload from Pi Zero\nTimestamp: {timestamp}\nThis is a test file."

    print(f"=== FTP Upload Test ===")
    print(f"Filename: {filename}")
    print(f"Content: {content}")
    print(f"Size: {len(content)} bytes\n")

    # Basic checks
    send_at("AT")
    send_at("AT+CSQ")
    send_at("AT+CREG?")

    # Activate network/PDP context
    send_at('AT+SAPBR=3,1,"Contype","GPRS"')
    send_at(f'AT+SAPBR=3,1,"APN","{cfg["apn"]}"')
    send_at('AT+SAPBR=1,1', wait_time=5)  # Open bearer
    send_at('AT+SAPBR=2,1')  # Query bearer - should show IP

    # Configure FTP
    send_at('AT+FTPCID=1')
    send_at('AT+FTPSERV="ftp.dlptest.com"')
    send_at('AT+FTPPORT=21')
    send_at('AT+FTPMODE=1')  # Passive mode
    send_at('AT+FTPUN="dlpuser"')
    send_at('AT+FTPPW="rNrKYTX9g7z3RgJRmxWuGHbeu"')
    send_at('AT+FTPTYPE="A"')  # ASCII mode
    send_at(f'AT+FTPPUTNAME="{filename}"')
    send_at('AT+FTPPUTPATH="/"')

    # Open FTP PUT session
    ser.reset_input_buffer()
    ser.write(b"AT+FTPPUT=1\r\n")
    print("> AT+FTPPUT=1")

    # Wait for +FTPPUT: 1,1,<maxlength> which means ready to receive data
    success, resp = wait_for_response("+FTPPUT: 1,1", timeout=60)
    if not success:
        print("Failed to open FTP session")
        return False

    # Send the data
    data_len = len(content)
    ser.reset_input_buffer()
    ser.write(f"AT+FTPPUT=2,{data_len}\r\n".encode())
    print(f"> AT+FTPPUT=2,{data_len}")

    # Wait for +FTPPUT: 2,<maxlength> prompt
    success, resp = wait_for_response("+FTPPUT: 2,", timeout=10)
    if not success:
        print("Failed to get data prompt")
        return False

    # Send the actual data
    ser.write(content.encode())
    print(f"> [sent {data_len} bytes]")

    # Wait for confirmation
    success, resp = wait_for_response("OK", timeout=10)

    # Close the session by sending 0 bytes
    time.sleep(1)
    send_at("AT+FTPPUT=2,0", wait_time=5)

    # Wait for final status
    success, resp = wait_for_response("+FTPPUT: 1,0", timeout=30)
    if success:
        print("\n=== Upload successful! ===")
    else:
        print("\n=== Upload may have completed, check server ===")

    return True

if __name__ == "__main__":
    test_ftp_upload()
    ser.close()