# Changelog

All notable changes to SPECTER firmware are documented here.

## [1.1.0] - Current

### Added
- **Ed25519 cryptographic signing** of ADVERT packets using rweather/Crypto library
- Deterministic key derivation from STM32 UID via SHA256
- Non-blocking receive mode using `startReceive()` + IRQ flag polling
- Comprehensive documentation in `docs/development-notes.md`
- Critical bootloader baud rate documentation (57600, not 115200)

### Changed
- Node identity now based on Ed25519 public key (not pseudo-random)
- Node name: `SPECTER-XXXX` derived from UID (e.g., `SPECTER-1811`)
- Memory usage: 51.7 KB flash (39.5%), 3.1 KB RAM (15.3%)
- Updated `flash.sh` and `platformio.ini` with correct 57600 baud rate

### Fixed
- **CRITICAL**: MeshCore companions now recognize the repeater (Ed25519 validation)
- **CRITICAL**: Receive no longer blocks forever (DIO1=RADIOLIB_NC polling fix)
- Radio configuration matches Heltec companion: 869.618 MHz, SF8, BW62.5 kHz

### Documentation
- Added `docs/development-notes.md`, critical lessons learned
- Updated `docs/protocol.md`, Ed25519 signature format
- Updated `docs/flashing.md`, 57600 baud requirement
- Updated `README.md`, dependencies, troubleshooting, Ed25519 overview

---

## [1.0.0] - Initial Release

### Added
- Basic MeshCore flood repeater implementation
- SX1262 driver via RadioLib 7.6.0
- STM32F103C8T6 support with CH340 USB-serial
- Auto-generated node name from UID
- Dedup cache (64 slots, circular buffer)
- ADVERT generation (every 12 hours + initial at boot)
- Flood relay with hop limiting and loop prevention

### Known Issues (Fixed in v1.1.0)
- ADVERT signature field was all zeros → companions ignored the node
- Blocking `radio.receive()` with DIO1=RADIOLIB_NC → no packets received
- Documentation stated 115200 baud → bootloader actually requires 57600

---

## Development Timeline

### Phase 1: Initial Hardware Bringup
- Pin mapping discovery from original DX-LR30 AT firmware
- RadioLib integration with SPI1 + RF switch pins
- Serial console via USART1 (ENABLE_HWSERIAL1 flag required)

### Phase 2: MeshCore Protocol Implementation
- Packet header parsing (route_type, payload_type, hop_count)
- Path hash tracking for loop prevention
- Dedup cache to avoid relay storms
- ADVERT payload format (flags, node name)

### Phase 3: Radio Parameter Tuning
- Discovered companion was configured at 869.618 MHz (not default 869.525)
- Confirmed SF=8, BW=62.5 kHz, CR=4/5, SyncWord=0x12

### Phase 4: Critical Fixes
- **Day 1**: "0 repeaters" in app → suspected radio params
- **Day 2**: Radio params matched, still not visible → suspected signature validation
- **Day 3**: Implemented Ed25519 signing → **REPEATER APPEARED!**
- **Day 4**: Discovered receive blocking issue → switched to polling mode
- **Day 5**: Flash failures at 115200 → discovered 57600 baud requirement

### Phase 5: Documentation & Polish
- Created comprehensive docs/ directory
- Documented all critical discoveries in development-notes.md
- Updated README with troubleshooting and Ed25519 overview
- Added warnings about baud rate and polling mode throughout codebase

---

## Versioning

This project follows [Semantic Versioning](https://semver.org/):
- MAJOR version: Breaking protocol or hardware changes
- MINOR version: New features, backward-compatible
- PATCH version: Bug fixes, documentation updates

---

## Contributing

See `docs/development-notes.md` for critical implementation details.

When reporting issues, include:
- Firmware version (check serial output on boot)
- Companion device and firmware version
- Radio parameters in use
- Serial monitor output showing the issue
- Whether `stm32flash -b 57600` succeeded or failed

---

## License

MIT License, see LICENSE file for details.
