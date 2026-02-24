#include "rs232_sender.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>


namespace {
	constexpr int kGpsEpochUnixDays = 3657;
	constexpr int kSecondsPerDay = 86400;
	constexpr int kSecondsPerWeek = 604800;

	[[nodiscard]] speed_t BaudToSpeed(const int baud) {
		switch (baud) {
			case 9600:
				return B9600;
			case 19200:
				return B19200;
			case 38400:
				return B38400;
			case 57600:
				return B57600;
			case 115200:
				return B115200;
			case 230400:
				return B230400;
			case 460800:
				return B460800;
			case 921600:
				return B921600;
			default:
				return B115200;
		}
	}
}  // namespace


RS232Sender::RS232Sender(const ForwarderConfig &config) :
	serial_port_(config.serial_port), baud_rate_(config.baud_rate), leap_seconds_(config.gps_utc_leap_seconds) {}


RS232Sender::~RS232Sender() {
	Close();
}


bool RS232Sender::Open() {
	if (fd_ >= 0) {
		return true;
	}

	fd_ = ::open(serial_port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd_ < 0) {
		std::fprintf(stderr, "[tod_forwarder] ERROR: open(%s) failed: %s\n", serial_port_.c_str(), std::strerror(errno));
		return false;
	}

	int flags = ::fcntl(fd_, F_GETFL, 0);
	if (flags >= 0) {
		::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
	}

	if (!ConfigurePort()) {
		Close();
		return false;
	}
	return true;
}


void RS232Sender::Close() {
	if (fd_ >= 0) {
		// Use poll to wait for output to drain with a timeout,
		// avoiding indefinite blocking from tcdrain().
		pollfd pfd{ fd_, POLLOUT, 0 };
		(void) ::poll(&pfd, 1, 200);
		::close(fd_);
		fd_ = -1;
	}
}


bool RS232Sender::IsOpen() const {
	return fd_ >= 0;
}


bool RS232Sender::ConfigurePort() const {
	struct termios tty{};
	if (::tcgetattr(fd_, &tty) != 0) {
		return false;
	}

	::cfmakeraw(&tty);

	const speed_t speed = BaudToSpeed(baud_rate_);
	::cfsetispeed(&tty, speed);
	::cfsetospeed(&tty, speed);

	tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
	tty.c_cflag |= CS8 | CLOCAL | CREAD;
	tty.c_cflag &= ~CRTSCTS;
	tty.c_iflag &= ~(IXON | IXOFF | IXANY);
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 0;

	if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
		return false;
	}

	struct serial_struct serial_info{};
	if (::ioctl(fd_, TIOCGSERIAL, &serial_info) == 0) {
		serial_info.flags |= ASYNC_LOW_LATENCY;
		::ioctl(fd_, TIOCSSERIAL, &serial_info);
	}

	::tcflush(fd_, TCIOFLUSH);
	return true;
}


void RS232Sender::GpsTimeToUtc(const std::uint16_t gps_week, const std::uint32_t gps_ms, const int leap_seconds, int &year, int &month,
							   int &day, int &hour, int &minute, int &second, int &centisecond) {
	const auto total_gps_sec = static_cast<std::int64_t>(gps_week) * kSecondsPerWeek + static_cast<std::int64_t>(gps_ms) / 1000;
	const std::int64_t total_utc_sec = total_gps_sec - leap_seconds;

	centisecond = static_cast<int>((gps_ms % 1000) / 10);

	const std::int64_t unix_sec = total_utc_sec + static_cast<std::int64_t>(kGpsEpochUnixDays) * kSecondsPerDay;
	std::int64_t remaining_sec = unix_sec % kSecondsPerDay;
	if (remaining_sec < 0) {
		remaining_sec += kSecondsPerDay;
	}

	hour = static_cast<int>(remaining_sec / 3600);
	minute = static_cast<int>((remaining_sec % 3600) / 60);
	second = static_cast<int>(remaining_sec % 60);

	auto total_days = static_cast<int>(unix_sec / kSecondsPerDay);
	if (unix_sec < 0 && (unix_sec % kSecondsPerDay) != 0) {
		--total_days;
	}

	constexpr int kDays1970To2000Mar1 = 11017;
	int d = total_days - kDays1970To2000Mar1;
	const int era = (d >= 0 ? d : d - 146096) / 146097;
	const int doe = d - era * 146097;
	const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int y = yoe + era * 400 + 2000;
	const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year = month <= 2 ? y + 1 : y;
}


std::uint8_t RS232Sender::NmeaChecksum(const char *begin, const std::size_t len) {
	std::uint8_t cs = 0;
	for (std::size_t i = 0; i < len; ++i) {
		cs ^= static_cast<std::uint8_t>(begin[i]);
	}
	return cs;
}


bool RS232Sender::SendGNZDA(const std::uint16_t gps_week, const std::uint32_t gps_millisecs) const {
	if (fd_ < 0) {
		return false;
	}

	char buf[64];
	int year, month, day, hour, minute, second, centisecond;
	GpsTimeToUtc(gps_week, gps_millisecs, leap_seconds_, year, month, day, hour, minute, second, centisecond);

	const int body_len = std::snprintf(buf + 1, sizeof(buf) - 1, "GNZDA,%02d%02d%02d.%02d,%02d,%02d,%04d,00,00", hour, minute, second,
									   centisecond, day, month, year);
	if (body_len <= 0 || body_len >= static_cast<int>(sizeof(buf) - 1)) {
		return false;
	}

	buf[0] = '$';
	const std::uint8_t cs = NmeaChecksum(buf + 1, static_cast<std::size_t>(body_len));

	const int tail_len = std::snprintf(buf + 1 + body_len, sizeof(buf) - 1 - body_len, "*%02X\r\n", cs);
	if (tail_len <= 0) {
		return false;
	}

	const std::size_t msg_len = 1 + body_len + tail_len;
	const ssize_t written = ::write(fd_, buf, msg_len);
	if (written < 0 || static_cast<std::size_t>(written) != msg_len) {
		return false;
	}

	return true;
}


bool RS232Sender::SendRaw(const char *data, const std::size_t len) const {
	if (fd_ < 0 || !data || len == 0) {
		return false;
	}

	const ssize_t written = ::write(fd_, data, len);
	if (written < 0 || static_cast<std::size_t>(written) != len) {
		return false;
	}

	return true;
}
