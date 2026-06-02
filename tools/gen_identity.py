#!/usr/bin/env python3
"""
gen_identity.py — Find an Ed25519 keypair whose pubkey[0] equals a target byte.
Usage:
  python3 gen_identity.py E5
  python3 gen_identity.py 30
  python3 gen_identity.py 2E
Output: privkey hex (to use as FIXED_PRIVKEY_HEX in platformio.ini)
"""
import os
import sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, PrivateFormat, NoEncryption

def find_key(target: int) -> tuple[bytes, bytes]:
    n = 0
    while True:
        seed = os.urandom(32)
        key  = Ed25519PrivateKey.from_private_bytes(seed)
        pub  = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        n += 1
        if pub[0] == target:
            return seed, pub, n

def main():
    if len(sys.argv) < 2:
        print("Usage: gen_identity.py <TARGET_BYTE_HEX>  e.g. E5")
        sys.exit(1)
    target = int(sys.argv[1], 16)
    print(f"Searching for pubkey[0] == 0x{target:02X}...")
    seed, pub, n = find_key(target)
    print(f"Found after {n} attempts!\n")
    print(f"  FIXED_PRIVKEY_HEX = {seed.hex()}")
    print(f"  pubkey            = {pub.hex()}")
    print(f"  pub[0]            = 0x{pub[0]:02X}")
    print(f"\nAdd to platformio.ini build_flags:")
    print(f'    -D FIXED_PRIVKEY_HEX=\\"{seed.hex()}\\"')

if __name__ == "__main__":
    main()
