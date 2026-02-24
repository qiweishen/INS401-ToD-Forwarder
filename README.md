### ToD Forwarder

ToD Forwarder is a small Linux utility that discovers Aceinna INS401 devices over raw Ethernet and forwards their Time‑of‑Day information to CoolShark AUTO66 V2 via RS‑232 serial port as NMEA time messages. It can either forward `$GNZDA` sentences directly from the device or generate compensated `$GPZDA` sentences from the binary GNSS solution packet, applying a configurable GPS–UTC leap second offset.

### Features

- **INS401 discovery over Ethernet**: Broadcasts a discovery request and listens on all active network interfaces for INS401 devices.
- **Raw Ethernet parsing**: Uses a custom `EthernetSocket` and INS401 protocol definitions to receive and validate Aceinna Ethernet frames.
- **Flexible time forwarding**:
  - With `use_gnss_packets` enabled: decodes GNSS solution packets, compensates leap seconds, and then sends formatted to AUTO66 V2 `$GPZDA` over RS‑232.
  - With `use_gnss_packets` disabled: directly forwards raw `$GNZDA` NMEA sentences from the INS401 to AUTO66 V2.
- **Low‑latency serial output**: Uses a tuned RS‑232 sender (`RS232Sender`) with configurable port and baud rate.

### Configuration

By default, the application reads its configuration from:

- **Default config file**: `/opt/qiweishen/tod_forwarder/config.txt`  
- **Override**: if use it via CLI, pass an alternative path as the first command‑line argument.

Supported keys in the config file:

- **`serial_port`**: RS‑232 device path, e.g. `/dev/ttyUSB0`
- **`baud_rate`**: serial baud rate, e.g. `115200`
- **`use_gnss_packets`**: `true`/`1` to enable GPS–UTC leap second compensation and generate `$GPZDA`; `false`/`0` to forward raw `$GNZDA`
- **`gps_utc_leap_seconds`**: integer GPS–UTC leap second offset (e.g. `18`)

Example `config.txt`:

```ini
# RS-232 output settings
serial_port = /dev/ttyUSB0
baud_rate = 115200

# Time-of-Day handling
use_gnss_packets = true
gps_utc_leap_seconds = 18
```

### Build

This project uses CMake.

- **Prerequisites**:
  - A C++17‑capable compiler (e.g. `g++` on Linux)
  - CMake
  - Linux with raw socket support (root or appropriate capabilities is typically required)

Typical build steps:

```bash
mkdir build
cd build
cmake ..
make
```

This will produce the `tod_forwarder` executable in the `build` directory (or similar, depending on your CMake configuration).

### Run

#### Basic usage:

```bash
# Using the default config path /opt/qiweishen/tod_forwarder/config.txt
sudo /opt/qiweishen/tod_forwarder/bin/tod_forwarder

# Using a custom config file (CLI)
sudo /opt/qiweishen/tod_forwarder/bin/tod_forwarder /path/to/config.txt
```

- **Discovery loop**: The program repeatedly discovers INS401 devices on all active non‑loopback interfaces, picks the first discovered device, and starts an `INSDeviceReceiver` for it.
- **Forwarding**: Depending on the configuration, the receiver either:
  - parses binary GNSS solution packets and sends `$GPZDA` via `RS232Sender`, or
  - validates and forwards `$GNZDA` NMEA sentences as‑is.
- **Termination**: Press `Ctrl+C` or send `SIGTERM`; the process will stop the receiver thread, close the serial port, and exit cleanly.

#### Run as a systemd service (autostart daemon)

To run ToD Forwarder as a systemd service and start it automatically at boot, use the provided `tod-forwarder.service` unit:

1. Copy the service file into the systemd unit directory:

```bash
sudo cp tod-forwarder.service /etc/systemd/system/tod-forwarder.service
```

2. Reload systemd to pick up the new unit:

```bash
sudo systemctl daemon-reload
```

3. Enable the service so it starts automatically on boot:

```bash
sudo systemctl enable tod-forwarder.service
```

4. Start the service immediately:

```bash
sudo systemctl start tod-forwarder.service
```

5. Check the status and logs if needed:

```bash
sudo systemctl status tod-forwarder.service
journalctl -u tod-forwarder.service -f
```

By default, the service runs:

```bash
/opt/qiweishen/tod_forwarder/bin/tod_forwarder /opt/qiweishen/tod_forwarder/config.txt
```

and requests the `CAP_NET_RAW` capability required for raw Ethernet sockets.

