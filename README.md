# ToD Forwarder

A lightweight Linux daemon that discovers **Aceinna INS401** devices over raw Ethernet and forwards their Time-of-Day information to **CoolShark AUTO66 V2** via RS-232 as NMEA time messages.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Build](#build)
- [Configuration](#configuration)
- [Run](#run)
  - [CLI Usage](#cli-usage)
  - [systemd Service (Autostart Daemon)](#systemd-service-autostart-daemon)
- [Testing](#testing)
  - [Hardware Setup](#hardware-setup)
  - [Build and Run Tests](#build-and-run-tests)
  - [Test Categories](#test-categories)
- [Deployment](#deployment)

---

## Overview

ToD Forwarder operates in one of two forwarding modes:

| Mode | Config Setting | Behaviour |
|------|---------------|-----------|
| **Direct forward** | `use_gnss_packets = false` | Validates and forwards raw `$GNZDA` NMEA sentences from the INS401 to AUTO66 V2 as-is. |
| **GNSS compensation** | `use_gnss_packets = true` | Decodes binary GNSS solution packets, applies a configurable GPS-UTC leap-second offset, generates `$GNZDA` sentences, and transmits them to AUTO66 V2 over RS-232. |

---

## Features

- **INS401 discovery over Ethernet** -- broadcasts a discovery request and listens on all active non-loopback network interfaces.
- **Raw Ethernet parsing** -- uses a custom `EthernetSocket` with BPF filtering and INS401 protocol definitions to receive and validate Aceinna Ethernet frames.
- **Flexible time forwarding** -- supports both direct NMEA passthrough and GPS-to-UTC converted output (see [Overview](#overview)).
- **Low-latency serial output** -- tuned RS-232 sender (`RS232Sender`) with `ASYNC_LOW_LATENCY`, configurable port and baud rate.
- **Graceful lifecycle** -- handles `SIGTERM`/`SIGINT` for clean shutdown; auto-rediscovery on device disconnect; restarts on failure as a systemd service.

---

## Project Structure

```
INS401-ToD-Forwarder/
├── include/
│   ├── ethernet_socket.h        # Raw Ethernet socket with BPF and epoll
│   ├── ins401_discover.h        # INS401 device discovery
│   ├── ins401_protocol.h        # Protocol constants and helpers
│   ├── ins401_receiver.h        # GNSS/NMEA frame receiver
│   └── rs232_sender.h           # RS-232 sender + GPS→UTC conversion
├── src/
│   ├── main.cpp                 # Entry point, config parsing, main loop
│   ├── ethernet_socket.cpp
│   ├── ins401_discover.cpp
│   ├── ins401_receiver.cpp
│   └── rs232_sender.cpp
├── tests/
│   └── test_serial_loopback.cpp # Serial loopback test suite (no INS401 needed)
├── 99-tod.rules                 # udev rule for /dev/ttyTOD symlink
├── tod-forwarder.service        # systemd service unit
├── tod_forwarder-config.txt     # Default configuration file
├── Amiga_Deployment.bash        # One-shot build + deploy script
├── CMakeLists.txt
├── LICENSE
└── README.md
```

---

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **Linux** | Raw socket support (`AF_PACKET`) is required. |
| **C++17 compiler** | e.g. `g++ >= 7` or `clang++ >= 5` |
| **CMake >= 3.16** | Build system. |
| **Root / `CAP_NET_RAW`** | Required for raw Ethernet sockets at runtime. |

---

## Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

This produces two executables in the `build/` directory:

| Binary | Purpose |
|--------|---------|
| `tod_forwarder` | Main daemon |
| `test_serial_loopback` | Serial loopback test suite |

---

## Configuration

The application reads a plain-text configuration file. If no path is provided on the command line, it defaults to `/opt/qiweishen/tod_forwarder-config.txt`.

### Supported Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `serial_port` | string | `/dev/ttyTOD` | RS-232 device path |
| `baud_rate` | integer | `115200` | Serial baud rate |
| `use_gnss_packets` | `true`/`false` | `false` | Enable GNSS compensation mode |
| `gps_utc_leap_seconds` | integer | `18` | GPS-UTC leap-second offset |

### Example `config.txt`

```ini
# RS-232 output settings
serial_port = /dev/ttyTOD
baud_rate = 115200

# Time-of-Day handling
use_gnss_packets = true
gps_utc_leap_seconds = 18
```

---

## Run

### CLI Usage

```bash
# Using the default config path
sudo ./build/tod_forwarder

# Using a custom config file
sudo ./build/tod_forwarder /path/to/config.txt
```

**Runtime behaviour:**

1. **Discovery loop** -- repeatedly broadcasts discovery requests on all active non-loopback interfaces, picks the first discovered INS401 device, and starts an `INSDeviceReceiver` for it.
2. **Forwarding** -- depending on `use_gnss_packets`:
   - `true`: parses binary GNSS solution packets and sends generated `$GNZDA` via RS-232.
   - `false`: validates and forwards `$GNZDA` NMEA sentences as-is.
3. **Termination** -- press `Ctrl+C` or send `SIGTERM`; the process stops the receiver thread, closes the serial port, and exits cleanly.

### systemd Service (Autostart Daemon)

The repository includes a ready-to-use systemd unit (`tod-forwarder.service`) and a udev rule (`99-tod.rules`).

**Service unit:**

```ini
[Unit]
Description=INS401 ToD Serial Forwarder for Auto66 V2
After=network.target

[Service]
Type=simple
ExecStart=/opt/qiweishen/tod_forwarder /opt/qiweishen/tod_forwarder-config.txt
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal
AmbientCapabilities=CAP_NET_RAW

[Install]
WantedBy=multi-user.target
```

**udev rule** (creates `/dev/ttyTOD` symlink for the USB-serial adapter):

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", ATTRS{serial}=="A10JEHMR", SYMLINK+="ttyTOD"
```

**Manual service management:**

```bash
sudo systemctl status  tod-forwarder.service
sudo systemctl restart tod-forwarder.service
journalctl -u tod-forwarder.service -f
```

---

## Testing

The test suite verifies serial data forwarding correctness **without requiring an INS401 device**. It uses a physical RS-232 loopback to exercise the `RS232Sender` class end-to-end: open/close, raw data integrity, `$GNZDA` generation, GPS-to-UTC conversion, NMEA checksum, and multi-message burst delivery.

### Hardware Setup

```
  Computer USB-A  ──►  USB-to-DB9 (male) Adapter  ──►  DB9 (male) Connector
                                                   ┌─────────┐
                                                   │ Pin 2 ──┤
                                                   │   ↕     │  (wire jumper)
                                                   │ Pin 3 ──┤
                                                   └─────────┘
```

![](./resource/rs232-pinout.webp)

- Connect a **USB-A to DB9 (male)** (RS-232) adapter to the computer.
- On the **DB9 (male) connector**, short **pin 2 (RXD)** to **pin 3 (TXD)** with a wire jumper to form a loopback.
- The adapter typically appears as `/dev/ttyUSB0`.

### Build and Run Tests

```bash
# Build (from repository root)
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Run with auto-detected port
sudo ./test_serial_loopback

# Or specify the port explicitly
sudo ./test_serial_loopback /dev/ttyUSB0
```

### Test Categories

| # | Category | Tests | What is verified |
|---|----------|-------|------------------|
| 1 | **RS232Sender class** | `open_close`, `open_invalid_port`, `sendraw_edge_cases`, `sendgnzda_closed_port` | Port lifecycle, error handling, edge-case rejection |
| 2 | **Raw data loopback** | `loopback_ascii`, `loopback_all_bytes`, `forward_nmea_sentence` | Byte-level integrity (0x00-0xFF), direct-forward mode simulation |
| 3 | **GNZDA + GPS-to-UTC** | `gps_epoch`, `week2348_leap18`, `week2348_no_leap`, `centiseconds`, `midday`, `end_of_week`, `late_january` | Time conversion correctness, NMEA `$GNZDA` format, checksum, leap-second compensation |
| 4 | **Multi-message** | `multiple_gnzda_burst` (10 msgs), `interleaved_raw_and_gnzda` | Burst delivery, message ordering, no data loss |
| 5 | **Baud rates** | `9600`, `38400`, `115200`, `230400` | Loopback at various serial speeds |
| 6 | **Latency benchmarks** | `latency_sendraw`, `latency_sendgnzda`, `latency_baud_comparison`, `latency_burst` | End-to-end forwarding delay (ms), baud-rate scaling, burst throughput |

### Latency Benchmarks

Category 6 measures real-world forwarding delay through the physical loopback. Each test records three timing points per message:

```
t0  = before SendGNZDA() / SendRaw()     (includes GPS→UTC conversion + formatting)
t1  = poll(POLLIN) returns                (first byte arrived back via loopback)
t2  = all bytes read                      (full sentence received)
```

| Metric | Formula | Meaning |
|--------|---------|---------|
| **First-byte latency** | t1 - t0 | Processing + kernel TX + USB round-trip + 1-byte wire time |
| **Full round-trip** | t2 - t0 | Above + remaining bytes on the wire |

In production the path is one-way (TX only to AUTO66 V2), so the **first-byte latency** is the most representative metric for real output delay.

**Theory time calculation:**

RS-232 with **8N1** framing transmits each data byte as a 10-bit frame:

```
[ 1 start bit | 8 data bits | 1 stop bit ]  =  10 bits per byte
```

The theoretical one-way wire time for **N** bytes at **B** baud is:

```
T = N × 10 / B   (seconds)
```

For a typical 38-byte `$GNZDA` sentence:

| Baud rate | Theory (one-way) | Calculation |
|-----------|------------------|-------------|
| 9600 | 39.58 ms | 38 × 10 / 9600 |
| 38400 | 9.90 ms | 38 × 10 / 38400 |
| 115200 | 3.30 ms | 38 × 10 / 115200 |
| 230400 | 1.65 ms | 38 × 10 / 230400 |

This is the absolute minimum time to push all bits onto the wire. Measured latency will be higher due to:

- CPU processing (GPS-to-UTC conversion, `snprintf`, NMEA checksum)
- Kernel serial driver buffering and scheduling
- USB controller round-trip (typically 1-2 ms for full-speed USB)
- Loopback return path (TX pin jumper to RX, electrically instant)

**Example output:**

```
============================================================
  ToD Forwarder  -  Serial Loopback Test Suite
  Port: /dev/ttyUSB0
  Hardware: USB-A to DB9 (male), pin 2 <-> pin 3 loopback
============================================================

-- RS232Sender class tests --
  [TEST] test_sender_open_close                    PASS
  [TEST] test_sender_open_invalid_port             PASS
  [TEST] test_sender_sendraw_edge_cases            PASS
  [TEST] test_sender_sendgnzda_closed_port         PASS
  ...

-- Latency benchmarks --
  [TEST] test_latency_sendraw                      PASS

    SendRaw() latency @ 115200 baud, 38-byte $GNZDA:
    First byte arrival:                  min=   3.50  avg=   4.12  med=   3.98  max=   6.21 ms  (50 samples)
    Full message round-trip:             min=   4.80  avg=   5.35  med=   5.20  max=   7.60 ms  (50 samples)
    Theory wire time (1-way, 8N1):       3.30 ms  (38 bytes * 10 bits / 115200 baud)

  [TEST] test_latency_baud_comparison              PASS

    Baud rate comparison – SendGNZDA() first-byte latency (8N1 framing):
    Baud          Min      Avg   Median      Max   Theory(1-way)
    ────────── ──────── ──────── ──────── ────────  ─────────────
    9600        40.10    41.25    40.85    43.70     39.58 ms
    38400       10.50    11.20    11.05    13.10      9.90 ms
    115200       3.50     4.10     3.95     6.20      3.30 ms
    230400       2.00     2.55     2.40     4.10      1.65 ms
  ...

============================================================
  Results:  25 passed,  0 failed,  25 total
============================================================
```

---

## Deployment

The `Amiga_Deployment.bash` script performs a complete one-shot build and install:

```bash
bash Amiga_Deployment.bash
```

**What it does:**

1. Cleans and rebuilds the project with CMake.
2. Installs the udev rule `99-tod.rules` to `/etc/udev/rules.d/` and reloads udev so `/dev/ttyTOD` appears.
3. Copies the binary to `/opt/qiweishen/` and sets `CAP_NET_RAW` capability.
4. Copies the default config `tod_forwarder-config.txt` to `/opt/qiweishen/`.
5. Installs, enables, and starts the `tod-forwarder.service` systemd unit.

**Post-deployment verification:**

```bash
ls -l /dev/ttyTOD
sudo systemctl status tod-forwarder.service
journalctl -u tod-forwarder.service -f
```

After this, ToD Forwarder starts automatically at boot and uses `/dev/ttyTOD` as its RS-232 output port.
