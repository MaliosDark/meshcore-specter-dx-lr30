#!/usr/bin/env python3
"""
flash_stm32.py, Flash STM32F103 via UART bootloader (8E1 protocol)
DX-LR30 CH340 wiring: DTR=BOOT0, RTS=NRST (active-low)
"""

import sys, time, struct, serial

PORT     = '/dev/ttyUSB0'
BAUD     = 115200
FW_PATH  = '/home/nexland/meshy/dx-lr30-fw/.pio/build/specter/firmware.bin'
FLASH_BASE = 0x08000000
CHUNK    = 256   # max 256 bytes per write

ACK  = 0x79
NACK = 0x1F

def checksum(data: bytes) -> int:
    cs = 0
    for b in data:
        cs ^= b
    return cs

class STM32Bootloader:
    def __init__(self, port: str, baud: int):
        self.s = serial.Serial(port, baud,
                               parity=serial.PARITY_EVEN,
                               bytesize=serial.EIGHTBITS,
                               stopbits=serial.STOPBITS_ONE,
                               timeout=2)

    def _boot_entry(self):
        """Assert RESET with BOOT0 high → releases into bootloader."""
        print("Entering bootloader...")
        self.s.dtr = True   # BOOT0 HIGH
        self.s.rts = False  # NRST LOW (assert)
        time.sleep(0.3)
        self.s.rts = True   # NRST HIGH (release) → MCU starts bootloader
        time.sleep(0.5)
        self.s.reset_input_buffer()

    def _expect_ack(self) -> bool:
        b = self.s.read(1)
        if not b:
            return False
        return b[0] == ACK

    def init(self) -> bool:
        self._boot_entry()
        for attempt in range(5):
            self.s.reset_input_buffer()
            self.s.write(b'\x7f')
            time.sleep(0.2)
            resp = self.s.read(2)
            if resp and (resp[0] == ACK or (len(resp) > 1 and resp[1] == ACK)):
                print("Bootloader init OK")
                return True
            # Retry with another pulse
            if attempt < 4:
                print(f"  Retry {attempt+1}...")
                self.s.rts = False
                time.sleep(0.1)
                self.s.rts = True
                time.sleep(0.3)
                self.s.reset_input_buffer()
        return False

    def get_id(self) -> int | None:
        self.s.write(bytes([0x02, 0x02^0xFF]))
        if not self._expect_ack():
            return None
        n = self.s.read(1)
        if not n:
            return None
        nbytes = n[0] + 1
        data = self.s.read(nbytes)
        if not self._expect_ack():
            return None
        return int.from_bytes(data, 'big')

    def erase_all(self) -> bool:
        print("Erasing flash...", end='', flush=True)
        self.s.write(bytes([0x43, 0x43^0xFF]))
        if not self._expect_ack():
            return False
        self.s.write(bytes([0xFF, 0x00]))
        self.s.timeout = 30
        ok = self._expect_ack()
        self.s.timeout = 2
        if ok:
            print(" done")
        return ok

    def write_mem(self, addr: int, data: bytes) -> bool:
        assert len(data) <= 256
        # Write Memory command
        self.s.write(bytes([0x31, 0x31^0xFF]))
        if not self._expect_ack():
            return False
        # Address + checksum
        ab = struct.pack('>I', addr)
        self.s.write(ab + bytes([checksum(ab)]))
        if not self._expect_ack():
            return False
        # Data: N-1, data bytes, checksum
        n = len(data)
        payload = bytes([n - 1]) + data
        cs = checksum(payload)
        self.s.write(payload + bytes([cs]))
        return self._expect_ack()

    def go(self, addr: int) -> bool:
        print(f"Starting app at 0x{addr:08X}...")
        self.s.write(bytes([0x21, 0x21^0xFF]))
        if not self._expect_ack():
            return False
        ab = struct.pack('>I', addr)
        self.s.write(ab + bytes([checksum(ab)]))
        return self._expect_ack()

    def close(self):
        self.s.close()

def main():
    with open(FW_PATH, 'rb') as f:
        firmware = f.read()

    # Pad to 256-byte boundary
    rem = len(firmware) % CHUNK
    if rem:
        firmware += b'\xFF' * (CHUNK - rem)

    print(f"Firmware: {len(firmware)} bytes")

    bl = STM32Bootloader(PORT, BAUD)

    if not bl.init():
        print("ERROR: Could not init bootloader. Check BOOT0/RESET wiring.")
        bl.close()
        sys.exit(1)

    chip_id = bl.get_id()
    if chip_id:
        print(f"Chip ID: 0x{chip_id:04X}")

    if not bl.erase_all():
        print("ERROR: Erase failed")
        bl.close()
        sys.exit(1)

    total = len(firmware)
    written = 0
    errors = 0
    print(f"Writing {total} bytes...")
    for offset in range(0, total, CHUNK):
        chunk = firmware[offset:offset+CHUNK]
        addr  = FLASH_BASE + offset
        if not bl.write_mem(addr, chunk):
            print(f"\nERROR at 0x{addr:08X}")
            errors += 1
            if errors > 3:
                break
        written += len(chunk)
        pct = written * 100 // total
        print(f"\r  {pct}% ({written}/{total})", end='', flush=True)

    print()
    if errors:
        print(f"Write failed with {errors} errors")
        bl.close()
        sys.exit(1)

    print("Write complete! Starting app...")
    bl.go(FLASH_BASE)
    bl.close()
    print("Done. Device is running new firmware.")
    print("Connect serial monitor at 115200 baud.")

if __name__ == '__main__':
    main()
