rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)

sudo rm -f /etc/udev/rules.d/99-tod.rules
sudo cp 99-tod.rules /etc/udev/rules.d/99-tod.rules
sudo udevadm control --reload-rules
sudo udevadm trigger

sudo systemctl stop tod-forwarder.service
sudo cp build/tod_forwarder /opt/qiweishen/
sudo cp tod_forwarder-config.txt /opt/qiweishen/
sudo cp tod-forwarder.service /etc/systemd/system/tod-forwarder.service
sudo systemctl daemon-reload
sudo systemctl enable tod-forwarder.service
sudo systemctl restart tod-forwarder.service
