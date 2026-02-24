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

- **`serial_port`**: RS‑232 device path, e.g. `/dev/ttyTOD`
- **`baud_rate`**: serial baud rate, e.g. `115200`
- **`use_gnss_packets`**: `true`/`1` to enable GPS–UTC leap second compensation and generate `$GPZDA`; `false`/`0` to forward raw `$GNZDA`
- **`gps_utc_leap_seconds`**: integer GPS–UTC leap second offset (e.g. `18`)

Example `config.txt`:

```ini
# RS-232 output settings
serial_port = /dev/ttyTOD
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
sudo /opt/qiweishen/tod_forwarder/build/tod_forwarder

# Using a custom config file (CLI)
sudo /opt/qiweishen/tod_forwarder/build/tod_forwarder /path/to/config.txt
```

- **Discovery loop**: The program repeatedly discovers INS401 devices on all active non‑loopback interfaces, picks the first discovered device, and starts an `INSDeviceReceiver` for it.
- **Forwarding**: Depending on the configuration, the receiver either:
  - parses binary GNSS solution packets and sends `$GPZDA` via `RS232Sender`, or
  - validates and forwards `$GNZDA` NMEA sentences as‑is.
- **Termination**: Press `Ctrl+C` or send `SIGTERM`; the process will stop the receiver thread, close the serial port, and exit cleanly.

#### Run as a systemd service (autostart daemon)

You can deploy and register ToD Forwarder as a systemd service so that it starts automatically at boot.  
The repository provides a helper script `Amiga_Deployment.bash` plus a udev rule `99-tod.rules` and a ready‑to‑use service unit `tod-forwarder.service`.

- **What the deployment script does (`Amiga_Deployment.bash`)**:
  - Builds the project with CMake into `build/` and produces the `tod_forwarder` binary.
  - Installs the udev rule `99-tod.rules` to `/etc/udev/rules.d/` so that the USB‑serial interface is exposed as `/dev/ttyTOD`:

    ```bash
    SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", ATTRS{serial}=="A10JEHMR", SYMLINK+="ttyTOD"
    ```

  - Reloads udev rules and triggers them so that `/dev/ttyTOD` appears.
  - Copies the built binary and config file to `/opt/qiweishen/`:

    ```bash
    sudo cp build/tod_forwarder /opt/qiweishen/
    sudo cp tod_forwarder-config.txt /opt/qiweishen/
    ```

  - Installs the systemd service unit:

    ```bash
    sudo cp tod-forwarder.service /etc/systemd/system/tod-forwarder.service
    sudo systemctl daemon-reload
    sudo systemctl enable tod-forwarder.service
    sudo systemctl restart tod-forwarder.service
    ```

- **Config file used by the service**:

  The default configuration installed by the script is `tod_forwarder-config.txt`:

  ```ini
  # INS401 ToD Serial Forwarder Configuration
  serial_port = /dev/ttyTOD
  baud_rate = 115200
  use_gnss_packets = false
  gps_utc_leap_seconds = 18
  ```

  - **`serial_port`** is set to the udev‑created symlink `/dev/ttyTOD`.
  - Adjust `use_gnss_packets` and `gps_utc_leap_seconds` as needed, then restart the service.

- **Service unit details (`tod-forwarder.service`)**:

  The installed systemd unit runs:

  ```bash
  /opt/qiweishen/tod_forwarder /opt/qiweishen/tod_forwarder-config.txt
  ```

  and requests the `CAP_NET_RAW` capability required for raw Ethernet sockets:

  ```ini
  [Service]
  Type=simple
  ExecStart=/opt/qiweishen/tod_forwarder /opt/qiweishen/tod_forwarder-config.txt
  Restart=on-failure
  RestartSec=3
  StandardOutput=journal
  StandardError=journal
  AmbientCapabilities=CAP_NET_RAW
  ```

- **Recommended one‑shot deployment flow**:

  From the repository root:

  ```bash
  # Build and deploy binary, config, udev rule and systemd service
  bash Amiga_Deployment.bash

  # Verify the device symlink and service
  ls -l /dev/ttyTOD
  sudo systemctl status tod-forwarder.service
  journalctl -u tod-forwarder.service -f
  ```

After this, ToD Forwarder will start automatically at boot as the `tod-forwarder.service` systemd unit and use `/dev/ttyTOD` as its RS‑232 output port.

