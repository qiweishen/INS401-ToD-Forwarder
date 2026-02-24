/// @file rs232_sender.h
/// @brief Low-latency RS-232 serial sender for NMEA $GNZDA Time-of-Day messages.

#ifndef TOD_FORWARDER_RS232_SENDER_H
#define TOD_FORWARDER_RS232_SENDER_H

#include <cstdint>
#include <string>


struct ForwarderConfig {
	std::string serial_port = "/dev/ttyTOD";
	int baud_rate = 115200;
	bool use_gnss_packets = false;
	int gps_utc_leap_seconds = 18;
};


class RS232Sender {
public:
	explicit RS232Sender(const ForwarderConfig &config);

	~RS232Sender();

	RS232Sender(const RS232Sender &) = delete;

	RS232Sender &operator=(const RS232Sender &) = delete;

	RS232Sender(RS232Sender &&) = delete;

	RS232Sender &operator=(RS232Sender &&) = delete;

	bool Open();

	void Close();

	[[nodiscard]] bool IsOpen() const;

	/// Format a $GNZDA sentence from GPS time and transmit it on the serial port.
	/// Returns true if the full message was written successfully.
	bool SendGNZDA(std::uint16_t gps_week, std::uint32_t gps_millisecs) const;

	/// Write raw bytes to the serial port without any formatting or compensation.
	bool SendRaw(const char *data, std::size_t len) const;

private:
	int fd_ = -1;
	std::string serial_port_;
	int baud_rate_;
	int leap_seconds_;

	[[nodiscard]] bool ConfigurePort() const;

	/// Convert GPS week number + milliseconds-of-week to UTC calendar fields.
	static void GpsTimeToUtc(std::uint16_t gps_week, std::uint32_t gps_ms, int leap_seconds, int &year, int &month, int &day,
							 int &hour, int &minute, int &second, int &centisecond);

	/// Compute NMEA-0183 checksum: XOR of all bytes between '$' (exclusive) and '*' (exclusive).
	static std::uint8_t NmeaChecksum(const char *begin, std::size_t len);
};


#endif	// TOD_FORWARDER_RS232_SENDER_H
