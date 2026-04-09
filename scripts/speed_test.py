#!/usr/bin/env python3
"""
SIM7000G upload speed test.

Run on Pi (stop service first):
  sudo systemctl stop airbridge && python3 ~/speed_test.py [--tcp-host HOST --tcp-port PORT]

Tests:
  1. FTP chunk sizes  — finds the modem's per-request cap
  2. Raw TCP socket   — bypasses FTP; shows air-interface throughput ceiling
     AT+CIPQSEND=1 used: SEND OK returns as soon as modem tx buffer is free,
     before waiting for server ACK, so this measures max upload rate.
"""
import os, sys, time, serial, yaml, argparse

CONFIG = os.path.expanduser('~/config.yaml')
FTP_TEST_SIZES = [1360, 2720, 4096, 8192, 16384, 32768, 65536]
TCP_TEST_SIZES = [512, 1024, 1460, 2048, 4096, 16384, 65536]
PAYLOAD = bytes(range(256)) * 1024  # 256 KB of test data


def wait(ser, tok, timeout=20):
    end = time.time() + timeout
    buf = ''
    while time.time() < end:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting).decode(errors='ignore')
            if tok in buf:
                return True, buf
            if 'ERROR' in buf:
                return False, buf
        else:
            time.sleep(0.01)
    return False, buf


def at(ser, cmd, tok='OK', t=5, quiet=False):
    ser.reset_input_buffer()
    ser.write((cmd + '\r\n').encode())
    ok, r = wait(ser, tok, t)
    if not quiet:
        status = 'OK' if ok else f'ERR ({r.strip()[:40]!r})'
        print(f'  {cmd[:55]:55s} → {status}')
    return ok, r


def setup_sapbr(ser, apn):
    print('\n[Network / SAPBR bearer]')
    at(ser, 'AT', quiet=True)
    at(ser, 'AT+CFUN=1', t=10, quiet=True)
    at(ser, 'AT+FTPSTOP', t=3, quiet=True)
    at(ser, 'AT+SAPBR=0,1', t=5, quiet=True)
    time.sleep(1)
    for i in range(15):
        ok, r = at(ser, 'AT+CREG?', t=2, quiet=True)
        if ',1' in r or ',5' in r:
            break
        print(f'  waiting for network ({i+1}/15)...')
        time.sleep(2)
    at(ser, 'AT+SAPBR=3,1,"Contype","GPRS"', quiet=True)
    at(ser, f'AT+SAPBR=3,1,"APN","{apn}"', quiet=True)
    ok, r = at(ser, 'AT+SAPBR=1,1', t=20, quiet=True)
    if not ok:
        print(f'  Bearer FAILED: {r.strip()}')
        return False
    print('  SAPBR bearer open OK')
    return True


def print_network_info(ser):
    print('\n[Network info]')
    _, r = at(ser, 'AT+CSQ', t=3, quiet=True)
    for line in r.splitlines():
        if '+CSQ' in line:
            print(f'  Signal:  {line.strip()}')
    _, r = at(ser, 'AT+CPSI?', t=3, quiet=True)
    for line in r.splitlines():
        if '+CPSI' in line:
            print(f'  Network: {line.strip()}')


def test_ftp_chunks(ser, ftp_cfg):
    print('\n[FTP chunk size test]')
    for cmd in [
        'AT+FTPCID=1',
        f'AT+FTPSERV="{ftp_cfg["server"]}"',
        f'AT+FTPPORT={ftp_cfg["port"]}',
        'AT+FTPMODE=1',
        f'AT+FTPUN="{ftp_cfg["username"]}"',
        f'AT+FTPPW="{ftp_cfg["password"]}"',
        'AT+FTPPUTNAME="speedtest.bin"',
        f'AT+FTPPUTPATH="{ftp_cfg.get("remote_path", "/")}"',
        'AT+FTPTYPE="I"',
        'AT+FTPPUTOPT="STOR"',
    ]:
        at(ser, cmd, quiet=True)

    ser.reset_input_buffer()
    ser.write(b'AT+FTPPUT=1\r\n')
    ok, resp = wait(ser, '+FTPPUT: 1,1', timeout=30)
    if not ok:
        print(f'  FTP session open FAILED: {resp!r}')
        return []

    modem_max = int(resp.strip().split(',')[-1])
    print(f'  Modem initial reported max: {modem_max} bytes')
    print()
    print(f'  {"Requested":>10}  {"Accepted":>10}  {"Time":>10}  {"KB/s":>10}')
    print(f'  {"-"*10}  {"-"*10}  {"-"*10}  {"-"*10}')

    results = []
    for req in FTP_TEST_SIZES:
        ser.reset_input_buffer()
        ser.write(f'AT+FTPPUT=2,{req}\r\n'.encode())
        ok, resp = wait(ser, '+FTPPUT: 2,', timeout=10)
        if not ok:
            print(f'  {req:>10}  {"REFUSED":>10}  (modem cap confirmed at {modem_max} B)')
            break
        try:
            accepted = int(resp.strip().split('+FTPPUT: 2,')[-1].split()[0])
        except Exception:
            accepted = modem_max

        t0 = time.time()
        ser.write(PAYLOAD[:accepted])
        wait(ser, 'OK', timeout=15)
        wait(ser, '+FTPPUT: 1,1', timeout=15)
        elapsed = time.time() - t0
        kbps = accepted / elapsed / 1024 if elapsed > 0 else 0
        results.append((req, accepted, elapsed, kbps))
        flag = '  ← CAPPED' if accepted < req else ''
        print(f'  {req:>10}  {accepted:>10}  {elapsed:>9.2f}s  {kbps:>9.1f}{flag}')
        if accepted < req:
            break

    at(ser, 'AT+FTPPUT=2,0', t=5, quiet=True)
    wait(ser, '+FTPPUT: 1,0', timeout=10)
    return results


def send_chunk_tcp(ser, data, timeout=30):
    """
    Send one chunk via AT+CIPSEND in CIPQSEND=1 mode.
    Returns (ok, elapsed_seconds).
    With CIPQSEND=1 the response token is 'DATA ACCEPT' not 'SEND OK'.
    """
    n = len(data)
    ser.reset_input_buffer()
    ser.write(f'AT+CIPSEND={n}\r\n'.encode())
    ok, _ = wait(ser, '>', timeout=10)
    if not ok:
        return False, 0.0
    t0 = time.time()
    ser.write(data)
    ok, _ = wait(ser, 'DATA ACCEPT', timeout=timeout)
    return ok, time.time() - t0


def test_raw_tcp(ser, host, port, apn):
    """
    Test raw TCP throughput via AT+CIPSTART + AT+CIPSEND.

    Strategy:
      1. Find the maximum bytes accepted per AT+CIPSEND (modem has a per-send cap).
      2. For valid sizes, send 64 KB worth of data in multiple calls and report
         aggregate throughput — this eliminates per-round-trip bias.
    AT+CIPQSEND=1: 'DATA ACCEPT:<n>' returns as soon as modem TX buffer is free,
    without waiting for server ACK.  This measures the air-interface uplink rate.
    """
    print(f'\n[Raw TCP test → {host}:{port}]')

    # Close SAPBR so we can open the raw TCP bearer
    at(ser, 'AT+SAPBR=0,1', t=5, quiet=True)
    time.sleep(1)

    # Set up raw TCP bearer (separate from SAPBR)
    at(ser, f'AT+CSTT="{apn}","",""', quiet=True)
    ok, _ = at(ser, 'AT+CIICR', t=20, quiet=True)
    if not ok:
        print('  AT+CIICR FAILED — skipping TCP test')
        return []
    _, r = at(ser, 'AT+CIFSR', t=5, quiet=True)
    for line in r.splitlines():
        if '.' in line and line.strip():
            print(f'  Modem IP: {line.strip()}')

    # Override DNS — carrier DNS may not resolve dynamic tunnel hostnames
    at(ser, 'AT+CDNSCFG="8.8.8.8","8.8.4.4"', quiet=True)

    # Quick-send mode: DATA ACCEPT returns after modem TX buffer drains (no server ACK wait)
    at(ser, 'AT+CIPQSEND=1', quiet=True)

    ok, r = at(ser, f'AT+CIPSTART="TCP","{host}",{port}', tok='CONNECT', t=30)
    if 'CONNECT OK' not in r and 'ALREADY CONNECT' not in r:
        print(f'  TCP connect FAILED: {r.strip()!r}')
        at(ser, 'AT+CIPSHUT', t=5, quiet=True)
        return []
    print('  TCP connected')

    # Drain any unsolicited data after CONNECT OK
    time.sleep(0.3)
    ser.read(ser.in_waiting)

    # --- Phase 1: find max bytes per CIPSEND ---
    print('\n  [Finding per-send size limit]')
    PROBE_SIZES = [512, 1024, 1460, 2048, 4096]
    max_chunk = 0
    for probe in PROBE_SIZES:
        ok, elapsed = send_chunk_tcp(ser, PAYLOAD[:probe])
        flag = 'OK' if ok else 'ERROR'
        print(f'    CIPSEND={probe:5d} → {flag}' + (f'  ({elapsed:.2f}s)' if ok else ''))
        if ok:
            max_chunk = probe
        else:
            break
    if max_chunk == 0:
        print('  All probe sizes failed — aborting TCP test')
        at(ser, 'AT+CIPCLOSE', t=5, quiet=True)
        at(ser, 'AT+CIPSHUT', t=5, quiet=True)
        return []
    print(f'\n  Max per-send: {max_chunk} bytes')

    # --- Phase 2: throughput test using max_chunk ---
    TARGET_BYTES = 65536  # send ~64 KB total to get a meaningful average
    n_sends = max(1, TARGET_BYTES // max_chunk)
    total_bytes = n_sends * max_chunk
    print(f'\n  [Throughput: {n_sends} × {max_chunk} B = {total_bytes} B total]')
    print(f'  {"Chunk":>6}  {"#Sends":>6}  {"Total B":>8}  {"Time":>8}  {"KB/s":>8}')
    print(f'  {"-"*6}  {"-"*6}  {"-"*8}  {"-"*8}  {"-"*8}')

    results = []
    t_start = time.time()
    sent = 0
    for i in range(n_sends):
        ok, _ = send_chunk_tcp(ser, PAYLOAD[:max_chunk])
        if not ok:
            print(f'    send #{i+1} failed — stopping')
            break
        sent += max_chunk
    elapsed = time.time() - t_start
    if sent > 0:
        kbps = sent / elapsed / 1024
        results.append((max_chunk, n_sends, sent, elapsed, kbps))
        print(f'  {max_chunk:>6}  {n_sends:>6}  {sent:>8}  {elapsed:>7.2f}s  {kbps:>7.1f}')

    at(ser, 'AT+CIPCLOSE', t=5, quiet=True)
    at(ser, 'AT+CIPSHUT', t=5, quiet=True)
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tcp-host', default=None, help='TCP tunnel host')
    parser.add_argument('--tcp-port', type=int, default=None, help='TCP tunnel port')
    parser.add_argument('--ftp-only', action='store_true')
    parser.add_argument('--tcp-only', action='store_true')
    args = parser.parse_args()

    with open(CONFIG) as f:
        cfg = yaml.safe_load(f)

    mc = cfg['modem']
    ser = serial.Serial(mc['port'], mc['baudrate'], timeout=1)
    print(f'Opened {mc["port"]} @ {mc["baudrate"]} baud')

    ftp_results = []
    tcp_results = []

    try:
        if not setup_sapbr(ser, mc['apn']):
            sys.exit(1)
        print_network_info(ser)

        if not args.tcp_only:
            ftp_results = test_ftp_chunks(ser, cfg['ftp'])

        if not args.ftp_only and args.tcp_host and args.tcp_port:
            tcp_results = test_raw_tcp(ser, args.tcp_host, args.tcp_port, mc['apn'])

    finally:
        at(ser, 'AT+FTPSTOP', t=3, quiet=True)
        at(ser, 'AT+CIPSHUT', t=3, quiet=True)
        at(ser, 'AT+SAPBR=0,1', t=5, quiet=True)
        ser.close()

    # Summary
    print('\n' + '=' * 50)
    print('SUMMARY')
    print('=' * 50)
    if ftp_results:
        best_ftp = max(ftp_results, key=lambda x: x[3])
        print(f'FTP best:     {best_ftp[1]:6d} B/chunk → {best_ftp[3]:.1f} KB/s')
    if tcp_results:
        # tcp_results tuples: (max_chunk, n_sends, total_bytes, elapsed, kbps)
        best_tcp = max(tcp_results, key=lambda x: x[4])
        print(f'TCP best:     {best_tcp[0]:6d} B/send × {best_tcp[1]} → {best_tcp[4]:.1f} KB/s  (CIPQSEND=1)')
        if ftp_results:
            ratio = best_tcp[4] / best_ftp[3]
            print(f'TCP speedup:  {ratio:.1f}x over FTP')

    print(f'\nRestart: sudo systemctl start airbridge')
