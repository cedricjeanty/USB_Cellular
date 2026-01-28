import serial
import time
import yaml

# Load configuration from shared config file
with open("config.yaml", "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

SERIAL_PORT = cfg['serial']['port']
BAUD_RATE = cfg['serial']['baudrate']
APN = cfg['apn']
CONFIGURE_MODEM = True  # Set to True to reset and configure modem settings

def send_at(ser, command, wait_time=1):
    ser.write((command + "\r\n").encode())
    time.sleep(wait_time)
    response = ser.read_all().decode('utf-8', errors='ignore')
    return response

def configure_modem(ser):
    print("--- Configuring Modem (Factory Reset & Setup) ---")
    print("Factory Reset:")
    print(send_at(ser, "AT&F"))         # Factory Reset
    print(send_at(ser, "AT&W"))         # Save Clean State
    print("Applying Settings:")
    print(send_at(ser, "AT+CSCLK=0"))     # Disable Sleep
    print(send_at(ser, "AT+CNETLIGHT=1")) # Turn on Status LED
    print(send_at(ser, "AT+IFC=0,0"))     # Disable Flow Control
    print(send_at(ser, "AT+IPR=115200"))  # Set Baud Rate
    print(send_at(ser, "AT&W"))           # Save Everything
    print("--- Configuration Done ---")

def main():
    print("Opening Serial Port...")
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except Exception as e:
        print(f"Error opening serial: {e}")
        return

    print("Checking Modem...")
    response = send_at(ser, "AT")
    print(response)
    if "OK" not in response:
        print("Error: Modem did not respond with OK. Check wiring and baud rate.")
        return

    if CONFIGURE_MODEM:
        configure_modem(ser)

    # 1. Ensure we are still on T-Mobile or Auto
    # (Optional: force T-Mobile if you find it drifting back to Verizon)
    # send_at(ser, 'AT+COPS=1,2,"310260"') 

    # 2. Check Registration
    print("Checking Network Registration...")
    for _ in range(10):
        resp = send_at(ser, "AT+CEREG?")
        print(resp)
        if ",1" in resp or ",5" in resp:
            print(" -> Registered!")
            break
        print(" -> Waiting for network...")
        time.sleep(2)
    
    # 3. Configure APN (Just to be safe)
    print("Configuring APN...")
    send_at(ser, f'AT+CGDCONT=1,"IPV4V6","{APN}"')
    
    # 4. Activate Data
    print("Activating Data Connection...")
    send_at(ser, f'AT+CNACT=1,"{APN}"', wait_time=3)
    
    # 5. Check IP
    resp = send_at(ser, "AT+CNACT?")
    if "0.0.0.0" in resp or "ERROR" in resp:
        print("Failed to get IP. Try rebooting modem.")
    else:
        print(f"IP Address Found: {resp.strip()}")
        
        # 6. Perform a tiny HTTP GET test
        print("Testing HTTP GET (simulating real data usage)...")
        send_at(ser, 'AT+HTTPINIT')
        send_at(ser, 'AT+HTTPPARA="CID",1')
        send_at(ser, 'AT+HTTPPARA="URL","http://httpbin.org/ip"')
        send_at(ser, 'AT+HTTPACTION=0', wait_time=5) # Wait for network
        print(send_at(ser, 'AT+HTTPREAD'))
        send_at(ser, 'AT+HTTPTERM')

    ser.close()

if __name__ == "__main__":
    main()