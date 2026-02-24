#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "ins401_discover.h"
#include "ins401_receiver.h"
#include "rs232_sender.h"


namespace {
    constexpr std::string_view kDefaultConfigPath = "/etc/tod_forwarder/config.txt";

    std::atomic<bool> g_terminate{false};

    void SignalHandler(int) {
        g_terminate.store(true, std::memory_order_release);
    }

    std::string Trim(const std::string &s) {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
    }

    ForwarderConfig ParseConfig(const std::string &path) {
        ForwarderConfig config;
        std::ifstream file(path);
        if (!file.is_open()) {
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
            }
            else if (key == "baud_rate") {
	            config.baud_rate = std::stoi(value);
            }
        	else if (key == "use_gnss_packets") {
	            config.use_gnss_packets = (value == "true" || value == "1");
			}
            else if (key == "gps_utc_leap_seconds") {
	            config.gps_utc_leap_seconds = std::stoi(value);
            }
        }
        return config;
    }
}


int main(int argc, char *argv[]) {
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    std::string config_path{kDefaultConfigPath};
    if (argc > 1) {
	    config_path = argv[1];
    }

    const ForwarderConfig config = ParseConfig(config_path);

    RS232Sender sender(config);
    if (!sender.Open()) {
	    return 1;
    }

    while (!g_terminate.load(std::memory_order_acquire)) {
        INSDeviceDiscover discover;
        auto devices = discover.DiscoverDevices();
        if (devices.empty()) {
            for (int i = 0; i < 5 && !g_terminate.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        const DeviceInfo &device = devices.begin()->second;

        auto receiver = std::make_unique<INSDeviceReceiver>(device.interface_name, device.mac_address);
    	if (config.use_gnss_packets) {
    		receiver->SetToDCallback([&sender](const std::uint16_t week, const std::uint32_t ms) {
				sender.SendGNZDA(week, ms);
			});
    	} else {
    		receiver->SetZDACallback([&sender](const char *data, const std::size_t len) {
				sender.SendRaw(data, len);
			});
    	}

        std::thread recv_thread([&receiver] { receiver->Run(); });

        while (!g_terminate.load(std::memory_order_acquire) && receiver->IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        receiver->Stop();
        if (recv_thread.joinable()) {
	        recv_thread.join();
        }
    }

    sender.Close();
    return 0;
}
