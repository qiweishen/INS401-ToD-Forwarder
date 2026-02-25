#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "ins401_discover.h"
#include "ins401_receiver.h"
#include "rs232_sender.h"


namespace {
	constexpr std::string_view kDefaultConfigPath = "/opt/qiweishen/tod_forwarder-config.txt";

	std::atomic<bool> g_terminate{ false };

	void SignalHandler(int) {
		// Second signal: force immediate exit.
		if (g_terminate.load(std::memory_order_relaxed)) {
			std::fprintf(stderr, "\n[tod_forwarder] forced exit\n");
			std::_Exit(1);
		}
		g_terminate.store(true, std::memory_order_release);
	}

	std::string Trim(const std::string &s) {
		const auto start = s.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return {};
		return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
	}

	int SafeStoi(const std::string &value, int fallback) {
		try {
			return std::stoi(value);
		} catch (const std::exception &) {
			std::fprintf(stderr, "[tod_forwarder] WARNING: invalid integer '%s', using default %d\n", value.c_str(), fallback);
			return fallback;
		}
	}

	ForwarderConfig ParseConfig(const std::string &path) {
		ForwarderConfig config;
		std::ifstream file(path);
		if (!file.is_open()) {
			std::fprintf(stderr, "[tod_forwarder] WARNING: cannot open config '%s', using defaults\n", path.c_str());
			return config;
		}

		std::string line;
		while (std::getline(file, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}

			std::string key = Trim(line.substr(0, eq));
			std::string value = Trim(line.substr(eq + 1));

			if (key == "serial_port") {
				config.serial_port = value;
			} else if (key == "baud_rate") {
				config.baud_rate = SafeStoi(value, config.baud_rate);
			} else if (key == "use_gnss_packets") {
				config.use_gnss_packets = (value == "true" || value == "1");
			} else if (key == "gps_utc_leap_seconds") {
				config.gps_utc_leap_seconds = SafeStoi(value, config.gps_utc_leap_seconds);
			}
		}
		return config;
	}
}  // namespace


int main(int argc, char *argv[]) {
	struct sigaction sa{};
	sa.sa_handler = SignalHandler;
	sa.sa_flags = 0;  // No SA_RESTART: let syscalls return EINTR so threads unblock.
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	std::string config_path{ kDefaultConfigPath };
	if (argc > 1) {
		config_path = argv[1];
	}

	std::fprintf(stderr, "[tod_forwarder] loading config from %s\n", config_path.c_str());
	const ForwarderConfig config = ParseConfig(config_path);
	std::fprintf(stderr, "[tod_forwarder] serial_port=%s baud=%d gnss_mode=%s leap_sec=%d\n", config.serial_port.c_str(),
				 config.baud_rate, config.use_gnss_packets ? "true" : "false", config.gps_utc_leap_seconds);

	RS232Sender sender(config);
	if (!sender.Open()) {
		std::fprintf(stderr, "[tod_forwarder] ERROR: failed to open serial port %s\n", config.serial_port.c_str());
		return 1;
	}
	std::fprintf(stderr, "[tod_forwarder] serial port %s opened\n", config.serial_port.c_str());

	while (!g_terminate.load(std::memory_order_acquire)) {
		INSDeviceDiscover discover(&g_terminate);
		auto devices = discover.DiscoverDevices();
		if (devices.empty()) {
			for (int i = 0; i < 5 && !g_terminate.load(std::memory_order_acquire); ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			continue;
		}

		const DeviceInfo &device = devices.begin()->second;
		std::fprintf(stderr, "[tod_forwarder] discovered device %s on %s\n", device.mac_address.c_str(),
					 device.interface_name.c_str());

		auto receiver = std::make_unique<INSDeviceReceiver>(device.interface_name, device.mac_address);
		if (config.use_gnss_packets) {
			receiver->SetToDCallback([&sender](const std::uint16_t week, const std::uint32_t ms) { sender.SendGNZDA(week, ms); });
		} else {
			receiver->SetZDACallback([&sender](const char *data, const std::size_t len) { sender.SendRaw(data, len); });
		}

		std::thread recv_thread([&receiver] { receiver->Run(); });

		while (!g_terminate.load(std::memory_order_acquire) && receiver->IsRunning()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		receiver->Stop();
		if (recv_thread.joinable()) {
			recv_thread.join();
		}
		std::fprintf(stderr, "[tod_forwarder] receiver stopped, rediscovering...\n");
	}

	std::fprintf(stderr, "[tod_forwarder] shutting down\n");
	sender.Close();
	return 0;
}
