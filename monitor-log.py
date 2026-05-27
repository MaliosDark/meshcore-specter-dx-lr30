#!/usr/bin/env python3
"""
Monitor serial port and log all output with timestamps.
Press Ctrl+C to stop.
"""

import serial
import sys
import time
from datetime import datetime
import os

PORT = '/dev/ttyUSB0'
BAUD = 115200
LOG_DIR = 'logs'
LOG_FILE = None

def get_log_filename():
    """Generate log filename with timestamp."""
    os.makedirs(LOG_DIR, exist_ok=True)
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    return os.path.join(LOG_DIR, f'specter_{timestamp}.log')

def log_line(f, line):
    """Write line to log file and stdout with timestamp."""
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
    log_entry = f"[{timestamp}] {line}"
    print(log_entry, flush=True)
    if f:
        f.write(log_entry + '\n')
        f.flush()

def main():
    global LOG_FILE
    LOG_FILE = get_log_filename()
    
    print(f"=== SPECTER Monitor/Logger ===")
    print(f"Port: {PORT} @ {BAUD}")
    print(f"Log:  {LOG_FILE}")
    print(f"Press Ctrl+C to stop\n")
    
    with open(LOG_FILE, 'w') as f:
        log_line(f, "=== Monitoring started ===")
        
        while True:
            try:
                # Open serial port
                ser = serial.Serial(PORT, BAUD, timeout=1)
                log_line(f, f"Connected to {PORT}")
                
                # Read loop
                while True:
                    try:
                        line = ser.readline()
                        if line:
                            decoded = line.decode('utf-8', errors='replace').rstrip()
                            log_line(f, decoded)
                    except serial.SerialException as e:
                        log_line(f, f"Serial error: {e}")
                        break
                    except UnicodeDecodeError:
                        log_line(f, f"<binary data: {line.hex()}>")
                
                ser.close()
                
            except serial.SerialException as e:
                log_line(f, f"Could not open {PORT}: {e}")
                log_line(f, "Retrying in 5 seconds...")
                time.sleep(5)
            
            except KeyboardInterrupt:
                log_line(f, "=== Monitoring stopped by user ===")
                print(f"\nLog saved to: {LOG_FILE}")
                sys.exit(0)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print(f"\nLog saved to: {LOG_FILE}")
        sys.exit(0)
