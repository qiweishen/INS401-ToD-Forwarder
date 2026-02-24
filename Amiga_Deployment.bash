#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -rf "$SCRIPT_DIR/build"
mkdir "$SCRIPT_DIR/build"
cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build"
make -C "$SCRIPT_DIR/build" -j"$(nproc)"

sudo rm -f /etc/udev/rules.d/99-tod.rules
sudo cp "$SCRIPT_DIR/99-tod.rules" /etc/udev/rules.d/99-tod.rules
sudo udevadm control --reload-rules
sudo udevadm trigger

sudo systemctl stop tod-forwarder.service || true
sudo cp "$SCRIPT_DIR/build/tod_forwarder" /opt/qiweishen/
sudo setcap cap_net_raw+ep /opt/qiweishen/tod_forwarder
sudo cp "$SCRIPT_DIR/tod_forwarder-config.txt" /opt/qiweishen/
sudo cp "$SCRIPT_DIR/tod-forwarder.service" /etc/systemd/system/tod-forwarder.service
sudo systemctl daemon-reload
sudo systemctl enable tod-forwarder.service
sudo systemctl restart tod-forwarder.service
