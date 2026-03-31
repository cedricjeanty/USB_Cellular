#!/usr/bin/env python3
"""CoolGear managed USB hub power control via serial protocol."""
import serial, sys, time

PORT = '/dev/ttyUSB0'
PW = 'pass    '  # 8 chars padded

def cmd(s, c):
    s.reset_input_buffer()
    s.write(c)
    time.sleep(0.3)
    return s.read(256).decode(errors='replace').strip()

def main():
    action = sys.argv[1] if len(sys.argv) > 1 else 'status'
    s = serial.Serial(PORT, 9600, timeout=1)
    
    if action == 'off':
        print(cmd(s, b'SP' + PW.encode() + b'00000000\r'))
    elif action == 'on':
        print(cmd(s, b'SP' + PW.encode() + b'FFFFFFFF\r'))
    elif action == 'cycle':
        delay = float(sys.argv[2]) if len(sys.argv) > 2 else 3
        print('OFF:', cmd(s, b'SP' + PW.encode() + b'00000000\r'))
        time.sleep(delay)
        print('ON:', cmd(s, b'SP' + PW.encode() + b'FFFFFFFF\r'))
    elif action == 'status':
        print('Query:', cmd(s, b'?Q\r'))
        print('Ports:', cmd(s, b'GP\r'))
    
    s.close()

if __name__ == '__main__':
    main()
