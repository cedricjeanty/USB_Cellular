# Developer Brief: USB Cellular Airbridge

**Environment:** Raspberry Pi Zero (W) running Raspberry Pi OS (Lite)

**Access:** `cedric@pizerologs.local` (scripts stored in `~`)

**Peripheral:** Waveshare SIM7000G (NB-IoT/Cat-M) HAT via UART (`/dev/ttyAMA0`)

**Hardware Interface:** USB-OTG port emulating a Mass Storage Device (USB Gadget)

## 0. Pi Zero Setup (Run Once)

### Deploy code to Pi:
```bash
scp *.py *.yaml *.sh cedric@pizerologs.local:~
```

### Run OTG setup script:
```bash
ssh cedric@pizerologs.local "sudo bash setup_otg.sh"
```

This configures:
- `/boot/firmware/config.txt`: Adds `dtoverlay=dwc2,dr_mode=peripheral` in `[all]` section
- `/etc/modules`: Adds `dwc2` for boot-time loading
- Uses DATA partition (`/dev/mmcblk0p3`) if available, otherwise creates `/piusb.bin`
- Creates mount point at `/mnt/usb_logs`

**Important:** The dwc2 overlay MUST be in the `[all]` section, not under `[cm4]` or `[cm5]`.

### Manual USB gadget commands:
```bash
# Load gadget (Pi appears as USB drive) - use partition or file
sudo modprobe g_mass_storage file=/dev/mmcblk0p3 stall=0 removable=1
# Or with file-backed image:
# sudo modprobe g_mass_storage file=/piusb.bin stall=0 removable=1

# Check UDC state
cat /sys/class/udc/*/state

# Unload gadget (before mounting locally)
sudo modprobe -r g_mass_storage

# Mount locally to read files
sudo mount -o loop /piusb.bin /mnt/usb_logs
```

## 1. Context & Objective

Develop a Python service that bridges a **Legacy Host (2004-era Linux)** to a cloud server. The Legacy Host treats the Pi as a standard USB thumb drive. The Pi must intermittently "harvest" files from this drive and upload them via cellular 4G (SIM7000G) to a remote server.

## 2. Technical Stack Requirements

* **Serial Comm:** `pyserial` for AT Commands to the SIM7000G.
* **System Operations:** `subprocess` for `modprobe`, `mount`, and `umount` commands.
* **File Management:** `os`, `shutil`, and `zipfile` for log handling.
* **Logging:** Robust local logging for debugging (since the device is headless).

## 3. Mandatory Logic Flow (The Duty Cycle)

The agent must implement a state machine with the following loop:

### Phase 1: Data Collection (Host Ownership)

1. Load `g_mass_storage` gadget pointing to `/piusb.bin`.
2. Monitor for "Quiet Window": Check `/sys/class/udc/$(ls /sys/class/udc)/state`.
3. If `state == 'configured'`, check file modification timestamp (`mtime`) on `/piusb.bin`.
4. Proceed to Phase 2 only if no writes have occurred for  minutes.

### Phase 2: Log Harvesting (Pi Ownership)

1. Unload USB Gadget: `sudo modprobe -r g_mass_storage`.
2. Mount virtual disk locally: `sudo mount -o loop /piusb.bin /mnt/usb_logs`.
3. Compress new files into a `.zip` or `.gz` archive.
4. Move archives to a `/home/cedric/outbox/` queue.
5. Unmount `/mnt/usb_logs`.

### Phase 3: Cellular Transmission (SIM7000G)

1. Initialize SIM7000G via UART.
2. **Challenge:** Files can be up to 100MB.
3. **Requirement:** Use **FTP(S)** or **Chunked HTTP POST** via AT Commands.
* *Agent must implement a chunking loop:* Read 512KB from file  `AT+HTTPDATA`  `AT+HTTPACTION`.


4. Implement **Automatic Resume/Retry**: If `AT+CSQ` (signal) is lost, pause and retry from the last successful chunk.
5. Upon 200 OK (Server Receipt), delete file from the outbox.

## 4. Specific Constraint Instructions

* **Single-Host Violation:** The code must **never** mount the loop device while `g_mass_storage` is active. This causes immediate filesystem corruption.
* **Power Resilience:** Use `os.sync()` after moving files. Assume power could be pulled at any moment.
* **AT Command Handling:** Do not use `time.sleep()` for modem responses. Use a proper serial read-until loop to wait for `OK`, `ERROR`, or `+HTTPACTION:`.

## 5. Deliverables

1. **`main.py`**: The primary state machine.
2. **`modem_handler.py`**: A class-based wrapper for SIM7000G AT commands (HTTP/FTP logic).
3. **`config.yaml`**: Configuration for APN, Server URL, Poll Interval, and Chunk Size.

configure the raspberry pi OTG port as a USB drive. wait for the file system to stop growing for 30s, and then unmount, and display the files that were added.

take a directory of files, and upload them through the cellular to an FTP server.