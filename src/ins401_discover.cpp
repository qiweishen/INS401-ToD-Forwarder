#include "ins401_discover.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <thread>

#include "ins401_protocol.h"


INSDeviceDiscover::INSDeviceDiscover(std::atomic<bool> *external_stop) : external_stop_(external_stop) {
	broadcast_mac_ = Ethernet::FormatMACAddress(std::string{ BROADCAST_MAC });
}

INSDeviceDiscover::~INSDeviceDiscover() = default;


std::map<std::string, DeviceInfo> INSDeviceDiscover::DiscoverDevices(int discovery_time_ms) {
	discovered_devices_.clear();

	const auto interfaces = Ethernet::GetNetworkInterfaces();
	if (interfaces.empty()) {
		return discovered_devices_;
	}

	running_.store(true);
	std::vector<std::thread> threads;
	threads.reserve(interfaces.size());
	for (const auto &interface: interfaces) {
		threads.emplace_back(
				[this, interface, discovery_time_ms]() { DiscoverOnInterface(interface.first, interface.second, discovery_time_ms); });
	}

	for (auto &t: threads) {
		t.join();
	}
	running_.store(false);

	return discovered_devices_;
}


void INSDeviceDiscover::DiscoverOnInterface(const std::string &interface, const std::string & /*local_mac_str*/,
											const int discovery_time_ms) {
	try {
		auto socket_ptr = std::make_shared<EthernetSocket>(interface, broadcast_mac_);
		if (!socket_ptr->IsValid()) {
			std::fprintf(stderr, "[tod_forwarder] WARNING: discovery socket invalid on %s\n", interface.c_str());
			return;
		}
		SendDiscoveryPing(socket_ptr);

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(discovery_time_ms);

		while (running_.load() && !(external_stop_ && external_stop_->load(std::memory_order_acquire)) &&
			   std::chrono::steady_clock::now() < deadline) {
			auto response = socket_ptr->Receive(100);
			if (response && !response->empty()) {
				HandleReceive(socket_ptr, response->data(), response->size());
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[tod_forwarder] WARNING: discovery on %s failed: %s\n", interface.c_str(), e.what());
	} catch (...) {
		std::fprintf(stderr, "[tod_forwarder] WARNING: discovery on %s failed with unknown error\n", interface.c_str());
	}
}


void INSDeviceDiscover::SendDiscoveryPing(const std::shared_ptr<EthernetSocket> &socket_ptr) const {
	std::vector<uint8_t> ping_packet =
			Ethernet::BuildAceinnaPacket(broadcast_mac_, socket_ptr->GetLocalMac(), REQUEST_INFO_COMMAND_BYTES, nullptr, 0);
	(void) socket_ptr->Send(ping_packet);
}


void INSDeviceDiscover::HandleReceive(const std::shared_ptr<EthernetSocket> &socket_ptr, const uint8_t *data, const size_t length) {
	ParseResponse(socket_ptr->GetInterface(), socket_ptr->GetLocalMac(), data, length);
}


bool INSDeviceDiscover::ParseResponse(const std::string &interface, const MacAddress &local_mac, const uint8_t *buffer,
									  const size_t len) {
	if (len < 60)
		return false;

	std::string device_mac = Ethernet::ParseMacAddress(buffer + kMacAddressSize);

	if (std::memcmp(buffer, broadcast_mac_.data(), kMacAddressSize) == 0)
		return false;

	if (buffer[kEthernetHeaderSize] != COMMAND_START_BYTES[0] || buffer[kEthernetHeaderSize + 1] != COMMAND_START_BYTES[1]) {
		return false;
	}

	if (buffer[kEthernetHeaderSize + 2] != REQUEST_INFO_COMMAND_BYTES[0] ||
		buffer[kEthernetHeaderSize + 3] != REQUEST_INFO_COMMAND_BYTES[1]) {
		return false;
	}

	uint32_t aceinna_payload_len = static_cast<uint32_t>(buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID]) |
								   (static_cast<uint32_t>(buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 1]) << 8) |
								   (static_cast<uint32_t>(buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 2]) << 16) |
								   (static_cast<uint32_t>(buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 3]) << 24);

	if (kEthernetHeaderSize + ACEINNA_HEADER_LEN + aceinna_payload_len + 2 > len)
		return false;

	uint16_t received_crc = static_cast<uint16_t>(buffer[kEthernetHeaderSize + ACEINNA_HEADER_LEN + aceinna_payload_len]) |
							(static_cast<uint16_t>(buffer[kEthernetHeaderSize + ACEINNA_HEADER_LEN + 1 + aceinna_payload_len]) << 8);
	uint16_t calculated_crc = Ethernet::CRC::CalculateINS401_CRC16(&buffer[kEthernetHeaderSize + 2], 6 + aceinna_payload_len);
	if (received_crc != calculated_crc)
		return false;

	DeviceInfo info;
	info.interface_name = interface;
	info.mac_address = device_mac;
	info.localhost_mac_address = Ethernet::ParseMacAddress(local_mac);

	if (aceinna_payload_len > 0) {
		std::string device_data(reinterpret_cast<const char *>(buffer + kEthernetHeaderSize + ACEINNA_HEADER_LEN),
								aceinna_payload_len);
		if (device_data.find(info.product) != std::string::npos) {
			std::istringstream iss(device_data);
			std::vector<std::string> tokens;
			std::string token;
			while (iss >> token) {
				tokens.push_back(token);
			}
			if (tokens.size() >= 18) {
				info.part_number = tokens[1];
				info.serial_number = tokens[2];
				info.hardware_version = tokens[4];
				info.imu_serial_number = tokens[6];
				info.firmware_version = tokens[7] + " " + tokens[8] + " " + tokens[9];
				info.bootloader_version = tokens[11];
				info.imu_firmware_version = tokens[12] + " " + tokens[13] + " " + tokens[14];
				info.gnss_chip_firmware_version = tokens[15] + " " + tokens[16] + " " + tokens[17];
			}
		}
	}

	{
		std::scoped_lock lock(devices_mutex_);
		discovered_devices_[device_mac] = info;
	}

	return true;
}
