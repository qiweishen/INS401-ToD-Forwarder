#include "ins401_receiver.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "ins401_protocol.h"


INSDeviceReceiver::INSDeviceReceiver(std::string iface, std::string device_mac) {
	MacAddress mac = Ethernet::FormatMACAddress(std::move(device_mac));
	socket_ptr_ = std::make_shared<EthernetSocket>(std::move(iface), mac, kBufferSize, true);
}


INSDeviceReceiver::~INSDeviceReceiver() {
	Stop();
}


void INSDeviceReceiver::Run() {
	if (!socket_ptr_ || !socket_ptr_->IsValid()) {
		std::fprintf(stderr, "[tod_forwarder] ERROR: receiver socket is invalid, cannot start\n");
		return;
	}
	running_.store(true);
	ReceiveLoop();
}


void INSDeviceReceiver::Stop() {
	running_.store(false);
}


void INSDeviceReceiver::ReceiveLoop() {
	while (running_.load()) {
		auto frames = socket_ptr_->ReceiveBatch(64);
		if (frames.empty()) {
			auto response = socket_ptr_->Receive(100);
			if (response && !response->empty()) {
				ProcessFrame(response->data(), response->size());
			}
		} else {
			for (const auto &frame: frames) {
				ProcessFrame(frame.data(), frame.size());
			}
		}
	}
}


void INSDeviceReceiver::ProcessFrame(const std::uint8_t *data, const std::size_t len) {
	if (len < kEthernetHeaderSize + ACEINNA_HEADER_LEN) {
		return;
	}

	const uint8_t *packet = data + kEthernetHeaderSize;
	const std::size_t payload_max = len - kEthernetHeaderSize;

	if (packet[0] == COMMAND_START_BYTES[0] && packet[1] == COMMAND_START_BYTES[1]) {
		const uint16_t msg_id = static_cast<uint16_t>(packet[2]) | (static_cast<uint16_t>(packet[3]) << 8);
		const uint32_t payload_len = static_cast<uint32_t>(packet[4]) | (static_cast<uint32_t>(packet[5]) << 8) |
									 (static_cast<uint32_t>(packet[6]) << 16) | (static_cast<uint32_t>(packet[7]) << 24);

		// Verify the frame is large enough to hold the full Aceinna packet (header + payload + CRC)
		if (ACEINNA_HEADER_LEN + payload_len + 2 > payload_max) {
			return;
		}

		if (msg_id == GNSS_SOLUTION_PACKET_MESSAGE_ID && payload_len == GNSS_SOLUTION_PACKET_LENGTH) {
			HandleGNSSPacket(packet);
		}
	} else if (packet[0] == '$') {
		HandleNMEA(packet, payload_max);
	}
}


void INSDeviceReceiver::HandleGNSSPacket(const std::uint8_t *packet) {
	constexpr std::size_t crc_offset = ACEINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = static_cast<uint16_t>(packet[crc_offset]) | (static_cast<uint16_t>(packet[crc_offset + 1]) << 8);
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + GNSS_SOLUTION_PACKET_LENGTH);
	if (recv_crc != calc_crc) {
		return;
	}

	const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
	uint16_t gps_week;
	uint32_t gps_ms;
	std::memcpy(&gps_week, payload, sizeof(uint16_t));
	std::memcpy(&gps_ms, payload + 2, sizeof(uint32_t));

	ToDCallback cb;
	{
		std::scoped_lock lock(cb_mutex_);
		cb = tod_callback_;
	}
	if (cb) {
		cb(gps_week, gps_ms);
	}
}


std::pair<char*, size_t> INSDeviceReceiver::FormateZDA(const char* input, size_t input_len) {
	// Find '*' position
	const char *asterisk = static_cast<const char *>(std::memchr(input, '*', input_len));
	size_t body_len = asterisk - input;

	// Allocate output buffer
	char* output = new char[input_len + 1];
	size_t output_len;

	// Check if there's a trailing comma before '*'
	if (input[body_len - 1] != ',') {
		std::memcpy(output, input, input_len);
		output_len = input_len;
		return {output, output_len};
	}

	// Copy body without the trailing comma
	size_t new_body_len = body_len - 1;
	std::memcpy(output, input, new_body_len);

	// Recalculate NMEA checksum: XOR of all bytes between '$' (exclusive) and '*' (exclusive)
	std::uint8_t calc_cs = 0;
	for (size_t i = 1; i < new_body_len; ++i) {
		calc_cs ^= static_cast<std::uint8_t>(output[i]);
	}

	// Append *XX\0
	std::snprintf(output + new_body_len, 5, "*%02X", calc_cs);
	output_len = new_body_len + 3;

	return {output, output_len};
}


void INSDeviceReceiver::HandleNMEA(const std::uint8_t *data, const std::size_t len) {
	const auto *str = reinterpret_cast<const char *>(data);

	// Find \r\n terminator
	const char *end = static_cast<const char *>(std::memchr(str, '\r', len));
	if (!end || (end - str + 1) >= static_cast<std::ptrdiff_t>(len) || end[1] != '\n') {
		return;
	}
	const std::size_t sentence_len = end - str + 2;	 // includes \r\n

	// Only forward $--ZDA sentences
	if (sentence_len < 7 || str[0] != '$' || std::memcmp(str + 3, "ZDA,", 4) != 0) {
		return;
	}

	// Find '*' for checksum validation
	const char *asterisk = static_cast<const char *>(std::memchr(str, '*', sentence_len));
	if (!asterisk || asterisk - str + 3 > static_cast<std::ptrdiff_t>(sentence_len)) {
		return;
	}

	// NMEA checksum: XOR of all bytes between '$' (exclusive) and '*' (exclusive)
	std::uint8_t calc_cs = 0;
	for (const char *p = str + 1; p < asterisk; ++p) {
		calc_cs ^= static_cast<std::uint8_t>(*p);
	}

	// Parse the two hex digits after '*'
	auto hex_val = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		return -1;
	};
	const int hi = hex_val(asterisk[1]);
	const int lo = hex_val(asterisk[2]);
	if (hi < 0 || lo < 0) {
		return;
	}
	const auto recv_cs = static_cast<std::uint8_t>((hi << 4) | lo);

	if (calc_cs != recv_cs) {
		return;
	}

	auto [zda, zda_len] = FormateZDA(str, sentence_len);

	NmeaZdaCallback cb;
	{
		std::scoped_lock lock(cb_mutex_);
		cb = nmea_zda_callback_;
	}
	if (cb) {
		cb(zda, zda_len);
	}
}
