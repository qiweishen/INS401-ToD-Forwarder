#ifndef INS_RECEIVER_H
#define INS_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ethernet_socket.h"


class INSDeviceReceiver {
public:
	INSDeviceReceiver(std::string iface, std::string device_mac);

	~INSDeviceReceiver();

	INSDeviceReceiver(const INSDeviceReceiver &) = delete;
	INSDeviceReceiver &operator=(const INSDeviceReceiver &) = delete;

	void Run();

	void Stop();

	[[nodiscard]] bool IsRunning() const { return running_.load(); }

	using ToDCallback = std::function<void(std::uint16_t gps_week, std::uint32_t gps_millisecs)>;
	using NmeaZdaCallback = std::function<void(const char *data, std::size_t len)>;

	void SetToDCallback(ToDCallback cb) {
		std::scoped_lock lock(cb_mutex_);
		tod_callback_ = std::move(cb);
	}

	void SetZDACallback(NmeaZdaCallback cb) {
		std::scoped_lock lock(cb_mutex_);
		nmea_zda_callback_ = std::move(cb);
	}

private:
	std::shared_ptr<EthernetSocket> socket_ptr_;
	std::atomic<bool> running_{ false };
	std::mutex cb_mutex_;
	ToDCallback tod_callback_;
	NmeaZdaCallback nmea_zda_callback_;

	static constexpr std::size_t kBufferSize = 64 * 1024;

	void ReceiveLoop();

	void ProcessFrame(const std::uint8_t *data, std::size_t len);

	void HandleGNSSPacket(const std::uint8_t *packet);

	static std::pair<char *, size_t> FormateStandardZDA(const char *input, size_t input_len);

	void HandleNMEA(const std::uint8_t *data, std::size_t len);
};


#endif
