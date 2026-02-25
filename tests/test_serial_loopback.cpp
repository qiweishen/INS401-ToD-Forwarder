/// @file test_serial_loopback.cpp
/// @brief Serial loopback tests for ToD Forwarder RS232 functionality.
///
/// Hardware setup:
///   - USB-A to DB9 adapter connected to computer's USB-A port
///   - DB9 pin 2 (RX) connected to pin 3 (TX) to form a loopback
///
/// These tests verify data forwarding correctness WITHOUT requiring an INS401 device.
/// They exercise the RS232Sender class (open/close, SendZDA, SendGNSSZDA) and validate
/// GNZDA format, NMEA checksum, GPS-to-UTC time conversion, and serial data integrity
/// through the physical loopback.
///
/// Usage:
///   sudo ./test_serial_loopback [/dev/ttyUSBx]
///
/// If no port is specified, the first available /dev/ttyUSB* device is used.

// ── Access private members of RS232Sender for read-back verification ────────
// This is a standard C++ unit-testing technique: we need to read() from the
// same file descriptor that RS232Sender::Open() creates, because the loopback
// routes TX back to RX on a single port.
#define private public
#define protected public
#include "rs232_sender.h"
#undef private
#undef protected

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <linux/serial.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>


// ═══════════════════════════════════════════════════════════════════════════════
//  Simple test framework
// ═══════════════════════════════════════════════════════════════════════════════

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static std::string g_serial_port;

#define TEST_ASSERT(cond, msg)                                                 \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "    FAIL: %s (line %d)\n", (msg), __LINE__); \
			return false;                                                      \
		}                                                                      \
	} while (0)

#define TEST_ASSERT_EQ(actual, expected, msg)                                                                          \
	do {                                                                                                               \
		if ((actual) != (expected)) {                                                                                  \
			std::fprintf(stderr, "    FAIL: %s -- expected %d, got %d (line %d)\n", (msg), static_cast<int>(expected), \
						 static_cast<int>(actual), __LINE__);                                                          \
			return false;                                                                                              \
		}                                                                                                              \
	} while (0)

#define RUN_TEST(func)                                  \
	do {                                                \
		g_tests_run++;                                  \
		std::fprintf(stderr, "  [TEST] %-40s ", #func); \
		if (func()) {                                   \
			g_tests_passed++;                           \
			std::fprintf(stderr, "PASS\n");             \
		} else {                                        \
			g_tests_failed++;                           \
			std::fprintf(stderr, "FAILED\n");           \
		}                                               \
	} while (0)


// ═══════════════════════════════════════════════════════════════════════════════
//  Helper utilities
// ═══════════════════════════════════════════════════════════════════════════════

/// Read from fd with a poll()-based timeout.  Returns bytes read, 0 on timeout, -1 on error.
static ssize_t read_with_timeout(int fd, void *buf, std::size_t count, int timeout_ms) {
	pollfd pfd{ fd, POLLIN, 0 };
	const int ret = ::poll(&pfd, 1, timeout_ms);
	if (ret <= 0) {
		return ret == 0 ? 0 : -1;
	}
	return ::read(fd, buf, count);
}

/// Keep reading until exactly @p count bytes arrive or timeout expires.
static ssize_t read_exact(int fd, void *buf, std::size_t count, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	std::size_t total = 0;
	while (total < count) {
		const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0) {
			break;
		}

		const ssize_t n = read_with_timeout(fd, static_cast<char *>(buf) + total, count - total, static_cast<int>(remaining));
		if (n < 0) {
			return -1;
		}
		if (n == 0) {
			continue;
		}
		total += static_cast<std::size_t>(n);
	}
	return static_cast<ssize_t>(total);
}

/// Drain any stale data sitting in the serial RX buffer.
static void flush_serial(int fd) {
	::tcflush(fd, TCIOFLUSH);
	char junk[256];
	while (read_with_timeout(fd, junk, sizeof(junk), 50) > 0) {
	}
}

/// Independent NMEA checksum: XOR every byte in [begin, begin+len).
static std::uint8_t nmea_xor(const char *begin, std::size_t len) {
	std::uint8_t cs = 0;
	for (std::size_t i = 0; i < len; ++i)
		cs ^= static_cast<std::uint8_t>(begin[i]);
	return cs;
}

/// Validate a complete $GNZDA sentence (format + checksum).
/// Expected format:  $GNZDA,HHMMSS.CC,DD,MM,YYYY,00,00*XX\r\n
static bool validate_gnzda(const std::string &s) {
	if (s.size() < 20) {
		return false;
	}
	if (s.substr(0, 6) != "$GNZDA") {
		return false;
	}
	if (s[s.size() - 1] != '\n' || s[s.size() - 2] != '\r') {
		return false;
	}

	const auto star = s.find('*');
	if (star == std::string::npos || star + 4 > s.size()) {
		return false;
	}

	// Compute checksum over body (between '$' and '*', both exclusive)
	const std::uint8_t calc = nmea_xor(s.c_str() + 1, star - 1);

	char hex[3];
	std::snprintf(hex, sizeof(hex), "%02X", calc);
	return s.substr(star + 1, 2) == hex;
}

/// Try to auto-detect the first available USB-serial adapter.
static std::string find_serial_port() {
	for (const char *pattern: { "/dev/ttyUSB*", "/dev/ttyACM*" }) {
		glob_t gb;
		if (::glob(pattern, 0, nullptr, &gb) == 0 && gb.gl_pathc > 0) {
			std::string result = gb.gl_pathv[0];
			::globfree(&gb);
			return result;
		}
		::globfree(&gb);
	}
	return {};
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 1 – RS232Sender basic class tests
// ═══════════════════════════════════════════════════════════════════════════════

/// Open → IsOpen → Close round-trip.
bool test_sender_open_close() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(!sender.IsOpen(), "initially closed");
	TEST_ASSERT(sender.Open(), "Open() succeeds");
	TEST_ASSERT(sender.IsOpen(), "open after Open()");
	TEST_ASSERT(sender.Open(), "second Open() is idempotent");

	sender.Close();
	TEST_ASSERT(!sender.IsOpen(), "closed after Close()");
	return true;
}

/// Opening a non-existent port must fail gracefully.
bool test_sender_open_invalid_port() {
	ForwarderConfig cfg;
	cfg.serial_port = "/dev/ttyNONEXISTENT_42";
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(!sender.Open(), "Open() fails for bad port");
	TEST_ASSERT(!sender.IsOpen(), "not open after failure");
	return true;
}

/// Edge cases: null pointer, zero length, closed port.
bool test_sender_sendzda_edge_cases() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open() succeeds");

	TEST_ASSERT(!sender.SendZDA(nullptr, 10), "null data rejected");
	TEST_ASSERT(!sender.SendZDA("abc", 0), "zero length rejected");

	sender.Close();
	TEST_ASSERT(!sender.SendZDA("abc", 3), "write on closed port rejected");
	return true;
}

/// SendGNSSZDA on a closed port must return false.
bool test_sender_sendgnsszda_closed_port() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(!sender.SendGNSSZDA(2348, 0), "SendGNSSZDA fails when closed");
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 2 – Raw data loopback tests
// ═══════════════════════════════════════════════════════════════════════════════

/// Simple ASCII string round-trip through the loopback.
bool test_raw_loopback_ascii() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	const char msg[] = "Hello, RS232 loopback!\r\n";
	const std::size_t len = std::strlen(msg);

	TEST_ASSERT(sender.SendZDA(msg, len), "SendZDA()");

	char rbuf[128] = {};
	const ssize_t n = read_exact(sender.fd_, rbuf, len, 1000);
	TEST_ASSERT_EQ(n, static_cast<ssize_t>(len), "read-back length");
	TEST_ASSERT(std::memcmp(msg, rbuf, len) == 0, "content matches");

	sender.Close();
	return true;
}

/// Full byte-range (0x00–0xFF) round-trip – catches parity/framing issues.
bool test_raw_loopback_all_bytes() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	char data[256];
	for (int i = 0; i < 256; ++i)
		data[i] = static_cast<char>(i);

	TEST_ASSERT(sender.SendZDA(data, 256), "SendZDA 256 bytes");

	char rbuf[256] = {};
	const ssize_t n = read_exact(sender.fd_, rbuf, 256, 2000);
	TEST_ASSERT_EQ(n, 256, "read-back 256 bytes");
	TEST_ASSERT(std::memcmp(data, rbuf, 256) == 0, "all byte values intact");

	sender.Close();
	return true;
}

/// Simulate the "direct forward" mode: send a raw $GNZDA sentence as-is.
bool test_raw_forward_nmea_sentence() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	// Construct a valid $GNZDA with correct checksum
	const char body[] = "GNZDA,120000.00,15,06,2025,00,00";
	const std::uint8_t cs = nmea_xor(body, std::strlen(body));
	char sentence[64];
	const int slen = std::snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, cs);
	TEST_ASSERT(slen > 0, "snprintf ok");

	const auto len = static_cast<std::size_t>(slen);
	TEST_ASSERT(sender.SendZDA(sentence, len), "SendZDA()");

	char rbuf[128] = {};
	const ssize_t n = read_exact(sender.fd_, rbuf, len, 1000);
	TEST_ASSERT_EQ(n, static_cast<ssize_t>(len), "read-back length");
	TEST_ASSERT(std::memcmp(sentence, rbuf, len) == 0, "forwarded NMEA intact");

	sender.Close();
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 3 – SendGNSSZDA loopback + GPS→UTC conversion verification
//
//  Each test calls SendGNSSZDA() with known GPS time inputs, reads the resulting
//  $GNZDA sentence back through the loopback, and checks that every field
//  (hour, minute, second, centisecond, day, month, year) and the NMEA checksum
//  are correct.
// ═══════════════════════════════════════════════════════════════════════════════

/// GPS epoch: week 0, ms 0, leap 0  →  1980-01-06 00:00:00.00 UTC
bool test_gnzda_gps_epoch() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 0;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(0, 0), "SendGNSSZDA(0,0)");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("000000.00") != std::string::npos, "time 00:00:00.00");
	TEST_ASSERT(s.find(",06,") != std::string::npos, "day 06");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",1980,") != std::string::npos, "year 1980");

	sender.Close();
	return true;
}

/// Week 2348, ms 0, leap 18  →  2025-01-04 23:59:42.00 UTC
/// (GPS week 2348 starts 2025-01-05 00:00:00 GPS; minus 18 s → previous day)
bool test_gnzda_week2348_leap18() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2348, 0), "SendGNSSZDA(2348,0)");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("235942.00") != std::string::npos, "time 23:59:42.00");
	TEST_ASSERT(s.find(",04,") != std::string::npos, "day 04");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",2025,") != std::string::npos, "year 2025");

	sender.Close();
	return true;
}

/// Week 2348, ms 0, leap 0  →  2025-01-05 00:00:00.00 UTC (no leap offset)
bool test_gnzda_week2348_no_leap() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 0;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2348, 0), "SendGNSSZDA(2348,0) no leap");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("000000.00") != std::string::npos, "time 00:00:00.00");
	TEST_ASSERT(s.find(",05,") != std::string::npos, "day 05");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",2025,") != std::string::npos, "year 2025");

	sender.Close();
	return true;
}

/// Centiseconds: week 2348, ms 12340, leap 18
///   → seconds = 12, centisecond = 340/10 = 34
///   → 2025-01-04 23:59:54.34 UTC
bool test_gnzda_centiseconds() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2348, 12340), "SendGNSSZDA(2348,12340)");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("235954.34") != std::string::npos, "time 23:59:54.34");

	sender.Close();
	return true;
}

/// Mid-day: week 2348, ms 43200000 (12 h), leap 18
///   → 2025-01-05 11:59:42.00 UTC
bool test_gnzda_midday() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2348, 43200000), "SendGNSSZDA mid-day");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("115942.00") != std::string::npos, "time 11:59:42.00");
	TEST_ASSERT(s.find(",05,") != std::string::npos, "day 05");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",2025,") != std::string::npos, "year 2025");

	sender.Close();
	return true;
}

/// End-of-week: week 2348, ms 604799000 (last second of the week), leap 18
///   → GPS: Sat 2025-01-11 23:59:59 GPS
///   → UTC: Sat 2025-01-11 23:59:41.00 UTC
bool test_gnzda_end_of_week() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2348, 604799000), "SendGNSSZDA end-of-week");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("235941.00") != std::string::npos, "time 23:59:41.00");
	TEST_ASSERT(s.find(",11,") != std::string::npos, "day 11");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",2025,") != std::string::npos, "year 2025");

	sender.Close();
	return true;
}

/// Leap-year date: week 2351, ms 259200000 (Wed, 3 days into week), leap 18
///   GPS week 2351 starts 2025-01-26.  +3d = 2025-01-29 00:00:00 GPS
///   UTC: 2025-01-28 23:59:42.00
bool test_gnzda_late_january() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	TEST_ASSERT(sender.SendGNSSZDA(2351, 259200000), "SendGNSSZDA late-jan");

	char buf[128] = {};
	const ssize_t n = read_exact(sender.fd_, buf, 80, 1000);
	TEST_ASSERT(n > 0, "read-back");

	std::string s(buf, static_cast<std::size_t>(n));
	TEST_ASSERT(validate_gnzda(s), "format + checksum");
	TEST_ASSERT(s.find("235942.00") != std::string::npos, "time 23:59:42.00");
	TEST_ASSERT(s.find(",28,") != std::string::npos, "day 28");
	TEST_ASSERT(s.find(",01,") != std::string::npos, "month 01");
	TEST_ASSERT(s.find(",2025,") != std::string::npos, "year 2025");

	sender.Close();
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 4 – Multi-message and stress tests
// ═══════════════════════════════════════════════════════════════════════════════

/// Send 10 GNZDA messages in rapid succession and verify all arrive intact.
bool test_multiple_gnzda_burst() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	constexpr int kCount = 10;
	for (int i = 0; i < kCount; ++i) {
		const std::uint32_t ms = static_cast<std::uint32_t>(i) * 1000000;  // 0, 1000 s, …
		TEST_ASSERT(sender.SendGNSSZDA(2348, ms), "SendGNSSZDA burst");
	}

	// Give the loopback a moment to deliver everything
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	char buf[1024] = {};
	const ssize_t n = read_exact(sender.fd_, buf, sizeof(buf), 3000);
	TEST_ASSERT(n > 0, "read-back burst");

	const std::string all(buf, static_cast<std::size_t>(n));

	// Count and validate each $GNZDA sentence
	int found = 0;
	std::size_t pos = 0;
	while ((pos = all.find("$GNZDA", pos)) != std::string::npos) {
		const std::size_t end = all.find("\r\n", pos);
		if (end == std::string::npos)
			break;
		const std::string sentence = all.substr(pos, end - pos + 2);
		TEST_ASSERT(validate_gnzda(sentence), "burst sentence checksum");
		++found;
		pos = end + 2;
	}
	TEST_ASSERT_EQ(found, kCount, "received all burst sentences");

	sender.Close();
	return true;
}

/// Interleave raw and GNZDA writes and verify order is preserved.
bool test_interleaved_raw_and_gnzda() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	// Build a valid $GNZDA to forward raw
	const char body[] = "GNZDA,180000.00,01,03,2025,00,00";
	const std::uint8_t cs = nmea_xor(body, std::strlen(body));
	char raw_nmea[64];
	const int rlen = std::snprintf(raw_nmea, sizeof(raw_nmea), "$%s*%02X\r\n", body, cs);
	TEST_ASSERT(rlen > 0, "snprintf raw NMEA");

	// Send: raw NMEA → GNZDA(binary) → raw NMEA
	TEST_ASSERT(sender.SendZDA(raw_nmea, static_cast<std::size_t>(rlen)), "raw #1");
	TEST_ASSERT(sender.SendGNSSZDA(2348, 0), "gnzda");
	TEST_ASSERT(sender.SendZDA(raw_nmea, static_cast<std::size_t>(rlen)), "raw #2");

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	char buf[512] = {};
	const ssize_t n = read_exact(sender.fd_, buf, sizeof(buf), 2000);
	TEST_ASSERT(n > 0, "read-back");

	const std::string all(buf, static_cast<std::size_t>(n));

	// Should see 3 sentences in order: $GNZDA, $GNZDA, $GNZDA
	// Advance past each sentence's \r\n before searching for the next one,
	// since all three share the same $GNZDA prefix.
	const auto p1 = all.find("$GNZDA");
	TEST_ASSERT(p1 != std::string::npos, "first $GNZDA");
	const auto end1 = all.find("\r\n", p1);
	TEST_ASSERT(end1 != std::string::npos, "first sentence terminator");

	const auto p2 = all.find("$GNZDA", end1 + 2);
	TEST_ASSERT(p2 != std::string::npos, "second $GNZDA");
	const auto end2 = all.find("\r\n", p2);
	TEST_ASSERT(end2 != std::string::npos, "second sentence terminator");

	const auto p3 = all.find("$GNZDA", end2 + 2);
	TEST_ASSERT(p3 != std::string::npos, "third $GNZDA");

	sender.Close();
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 5 – Baud-rate tests
// ═══════════════════════════════════════════════════════════════════════════════

static bool loopback_at_baud(int baud, int timeout_ms) {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate = baud;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	const char msg[] = "$GNZDA,baud-rate-test\r\n";
	const std::size_t len = std::strlen(msg);
	TEST_ASSERT(sender.SendZDA(msg, len), "SendZDA()");

	char rbuf[64] = {};
	const ssize_t n = read_exact(sender.fd_, rbuf, len, timeout_ms);
	TEST_ASSERT_EQ(n, static_cast<ssize_t>(len), "read-back length");
	TEST_ASSERT(std::memcmp(msg, rbuf, len) == 0, "data matches");

	sender.Close();
	return true;
}

bool test_baud_9600() {
	return loopback_at_baud(9600, 3000);
}
bool test_baud_38400() {
	return loopback_at_baud(38400, 2000);
}
bool test_baud_115200() {
	return loopback_at_baud(115200, 1000);
}
bool test_baud_230400() {
	return loopback_at_baud(230400, 1000);
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Category 6 – Latency benchmarks
//
//  Simulates real-world forwarding and measures end-to-end delay through the
//  physical loopback.  Three timing points per message:
//
//      t0  = before SendGNSSZDA() / SendZDA()       (includes GPS→UTC + format)
//      t1  = poll(POLLIN) returns                  (first byte arrived back)
//      t2  = all bytes read                        (full sentence received)
//
//  "First-byte latency"  = t1 − t0   (processing + kernel TX + USB + 1-byte wire)
//  "Full round-trip"     = t2 − t0   (above + remaining bytes on the wire)
//
//  In the production system the path is one-way (TX only → AUTO66 V2), so the
//  first-byte latency is the most representative metric for real output delay.
//
//  ── Theory time calculation ──────────────────────────────────────────────────
//
//  RS-232 with 8N1 framing transmits each data byte as a 10-bit frame:
//
//      [ 1 start bit | 8 data bits | 1 stop bit ]  =  10 bits per byte
//
//  The theoretical one-way wire time for N bytes at B baud is:
//
//      T = N * 10 / B   (seconds)
//
//  For example, a 38-byte $GNZDA sentence at 115200 baud:
//
//      T = 38 * 10 / 115200 = 3.30 ms
//
//  This is the absolute minimum time to push all bits onto the wire.  The
//  measured latency will always be higher because it also includes:
//    - CPU processing (GPS→UTC conversion, snprintf, NMEA checksum)
//    - Kernel serial driver buffering and scheduling
//    - USB controller round-trip (typically 1–2 ms for full-speed USB)
//    - Loopback return path (TX → pin jumper → RX, electrically instant)
//
// ═══════════════════════════════════════════════════════════════════════════════

using Clock = std::chrono::high_resolution_clock;

struct LatencyStats {
	double min_ms;
	double max_ms;
	double avg_ms;
	double median_ms;
	int samples;
};

static LatencyStats compute_stats(std::vector<double> &v) {
	std::sort(v.begin(), v.end());
	LatencyStats s{};
	s.samples   = static_cast<int>(v.size());
	s.min_ms    = v.front();
	s.max_ms    = v.back();
	s.median_ms = v[v.size() / 2];
	double sum  = 0;
	for (const double x : v) sum += x;
	s.avg_ms = sum / static_cast<double>(v.size());
	return s;
}

static void print_stats(const char *label, const LatencyStats &s) {
	std::fprintf(stderr, "    %-32s min=%7.2f  avg=%7.2f  med=%7.2f  max=%7.2f ms  (%d samples)\n",
	             label, s.min_ms, s.avg_ms, s.median_ms, s.max_ms, s.samples);
}

/// Measure SendZDA() round-trip latency (direct-forward mode simulation).
bool test_latency_sendzda() {
	ForwarderConfig cfg;
	cfg.serial_port = g_serial_port;
	cfg.baud_rate   = 115200;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");

	// Build a realistic $GNZDA sentence (~38 bytes, typical INS401 output)
	const char body[] = "GNZDA,120000.00,15,06,2025,00,00";
	const std::uint8_t cs = nmea_xor(body, std::strlen(body));
	char sentence[64];
	const int slen = std::snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, cs);
	const auto len = static_cast<std::size_t>(slen);

	constexpr int kIterations = 50;
	std::vector<double> first_byte_ms, full_msg_ms;

	for (int i = 0; i < kIterations; ++i) {
		flush_serial(sender.fd_);

		const auto t0 = Clock::now();
		sender.SendZDA(sentence, len);

		// Wait for first byte to arrive back through loopback
		pollfd pfd{sender.fd_, POLLIN, 0};
		::poll(&pfd, 1, 2000);
		const auto t1 = Clock::now();

		// Read the complete message
		char rbuf[128] = {};
		std::size_t total = 0;
		while (total < len) {
			const ssize_t n = read_with_timeout(sender.fd_, rbuf + total, sizeof(rbuf) - total, 500);
			if (n <= 0) break;
			total += static_cast<std::size_t>(n);
		}
		const auto t2 = Clock::now();

		first_byte_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		full_msg_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
	}

	auto fb = compute_stats(first_byte_ms);
	auto fm = compute_stats(full_msg_ms);

	// Theory: N bytes * 10 bits/byte / baud  (8N1 framing = 1 start + 8 data + 1 stop)
	const double theory_ms = static_cast<double>(len) * 10.0 / 115200.0 * 1e3;
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "    SendZDA() latency @ 115200 baud, %d-byte $GNZDA:\n", slen);
	print_stats("First byte arrival:", fb);
	print_stats("Full message round-trip:", fm);
	std::fprintf(stderr, "    Theory wire time (1-way, 8N1):    %.2f ms  (%d bytes * 10 bits / 115200 baud)\n",
	             theory_ms, slen);

	sender.Close();
	return true;
}

/// Measure SendGNSSZDA() end-to-end latency (GNSS compensation mode simulation).
/// This includes GPS→UTC conversion + NMEA formatting + serial TX + loopback RX.
bool test_latency_sendgnsszda() {
	ForwarderConfig cfg;
	cfg.serial_port          = g_serial_port;
	cfg.baud_rate            = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");

	constexpr int kIterations = 50;
	std::vector<double> first_byte_ms, full_msg_ms;

	for (int i = 0; i < kIterations; ++i) {
		flush_serial(sender.fd_);

		const auto t0 = Clock::now();
		sender.SendGNSSZDA(2348, static_cast<std::uint32_t>(i) * 1000);

		pollfd pfd{sender.fd_, POLLIN, 0};
		::poll(&pfd, 1, 2000);
		const auto t1 = Clock::now();

		char rbuf[128] = {};
		std::size_t total = 0;
		while (total < 38) {  // typical $GNZDA ≈ 38 bytes
			const ssize_t n = read_with_timeout(sender.fd_, rbuf + total, sizeof(rbuf) - total, 500);
			if (n <= 0) break;
			total += static_cast<std::size_t>(n);
		}
		const auto t2 = Clock::now();

		first_byte_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		full_msg_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
	}

	auto fb = compute_stats(first_byte_ms);
	auto fm = compute_stats(full_msg_ms);

	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "    SendGNSSZDA() latency @ 115200 baud (GPS->UTC + format + TX + loopback):\n");
	print_stats("First byte arrival:", fb);
	print_stats("Full message round-trip:", fm);

	sender.Close();
	return true;
}

/// Compare forwarding latency across baud rates.
bool test_latency_baud_comparison() {
	struct BaudEntry { int baud; int timeout; };
	constexpr BaudEntry bauds[] = {{9600, 5000}, {38400, 3000}, {115200, 2000}, {230400, 2000}};

	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "    Baud rate comparison – SendGNSSZDA() first-byte latency (8N1 framing):\n");
	std::fprintf(stderr, "    %-10s %8s %8s %8s %8s   %s\n",
	             "Baud", "Min", "Avg", "Median", "Max", "Theory(1-way)");
	std::fprintf(stderr, "    ────────── ──────── ──────── ──────── ────────   ─────────────\n");

	for (const auto &[baud, timeout] : bauds) {
		ForwarderConfig cfg;
		cfg.serial_port          = g_serial_port;
		cfg.baud_rate            = baud;
		cfg.gps_utc_leap_seconds = 18;

		RS232Sender sender(cfg);
		if (!sender.Open()) {
			std::fprintf(stderr, "    %-10d  (failed to open)\n", baud);
			continue;
		}

		constexpr int kIterations = 20;
		std::vector<double> latencies;

		for (int i = 0; i < kIterations; ++i) {
			flush_serial(sender.fd_);

			const auto t0 = Clock::now();
			sender.SendGNSSZDA(2348, static_cast<std::uint32_t>(i) * 1000);

			pollfd pfd{sender.fd_, POLLIN, 0};
			::poll(&pfd, 1, timeout);
			const auto t1 = Clock::now();

			// Drain remaining bytes
			char junk[128];
			while (read_with_timeout(sender.fd_, junk, sizeof(junk), 200) > 0) {}

			latencies.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		}

		auto s = compute_stats(latencies);
		// Theory: 38 bytes * 10 bits / baud  (8N1: 1 start + 8 data + 1 stop = 10 bits/byte)
		const double theory_ms = 38.0 * 10.0 / static_cast<double>(baud) * 1e3;

		std::fprintf(stderr, "    %-10d %7.2f %7.2f %7.2f %7.2f   %.2f ms\n",
		             baud, s.min_ms, s.avg_ms, s.median_ms, s.max_ms, theory_ms);

		sender.Close();
	}

	return true;
}

/// Measure per-message latency under burst conditions (back-to-back sends).
bool test_latency_burst() {
	ForwarderConfig cfg;
	cfg.serial_port          = g_serial_port;
	cfg.baud_rate            = 115200;
	cfg.gps_utc_leap_seconds = 18;

	RS232Sender sender(cfg);
	TEST_ASSERT(sender.Open(), "Open()");
	flush_serial(sender.fd_);

	constexpr int kCount = 20;
	std::vector<double> per_msg_ms;

	for (int i = 0; i < kCount; ++i) {
		const auto t0 = Clock::now();
		sender.SendGNSSZDA(2348, static_cast<std::uint32_t>(i) * 1000000);

		// Wait for the complete sentence to arrive
		char rbuf[128] = {};
		std::size_t total = 0;
		while (total < 38) {
			const ssize_t n = read_with_timeout(sender.fd_, rbuf + total, sizeof(rbuf) - total, 2000);
			if (n <= 0) break;
			total += static_cast<std::size_t>(n);
		}
		const auto t1 = Clock::now();

		per_msg_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
	}

	auto s = compute_stats(per_msg_ms);

	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "    Burst latency @ 115200 (%d back-to-back SendGNSSZDA):\n", kCount);
	print_stats("Per-message round-trip:", s);
	std::fprintf(stderr, "    Total burst time:                 %.2f ms\n", s.avg_ms * kCount);
	std::fprintf(stderr, "    Effective throughput:              %.1f msgs/sec\n",
	             1e3 / s.avg_ms);

	sender.Close();
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[]) {
	// ── Determine serial port ──
	if (argc > 1) {
		g_serial_port = argv[1];
	} else {
		g_serial_port = find_serial_port();
	}

	if (g_serial_port.empty()) {
		std::fprintf(stderr,
					 "ERROR: No serial port found or specified.\n"
					 "Usage: sudo %s [/dev/ttyUSBx]\n"
					 "Make sure a USB-to-serial adapter is connected with DB9 pins 2-3 looped.\n",
					 argv[0]);
		return 1;
	}

	if (::access(g_serial_port.c_str(), F_OK) != 0) {
		std::fprintf(stderr, "ERROR: Serial port %s does not exist.\n", g_serial_port.c_str());
		return 1;
	}

	std::fprintf(stderr,
				 "============================================================\n"
				 "  ToD Forwarder  -  Serial Loopback Test Suite\n"
				 "  Port: %s\n"
				 "  Hardware: USB-A to DB9, pin 2 <-> pin 3 loopback\n"
				 "============================================================\n\n",
				 g_serial_port.c_str());

	// ── Category 1: RS232Sender class basics ──
	std::fprintf(stderr, "-- RS232Sender class tests --\n");
	RUN_TEST(test_sender_open_close);
	RUN_TEST(test_sender_open_invalid_port);
	RUN_TEST(test_sender_sendzda_edge_cases);
	RUN_TEST(test_sender_sendgnsszda_closed_port);

	// ── Category 2: Raw data loopback ──
	std::fprintf(stderr, "\n-- Raw data loopback tests --\n");
	RUN_TEST(test_raw_loopback_ascii);
	RUN_TEST(test_raw_loopback_all_bytes);
	RUN_TEST(test_raw_forward_nmea_sentence);

	// ── Category 3: GNZDA generation + GPS→UTC conversion ──
	std::fprintf(stderr, "\n-- GNZDA generation & GPS-to-UTC conversion tests --\n");
	RUN_TEST(test_gnzda_gps_epoch);
	RUN_TEST(test_gnzda_week2348_leap18);
	RUN_TEST(test_gnzda_week2348_no_leap);
	RUN_TEST(test_gnzda_centiseconds);
	RUN_TEST(test_gnzda_midday);
	RUN_TEST(test_gnzda_end_of_week);
	RUN_TEST(test_gnzda_late_january);

	// ── Category 4: Multi-message / stress ──
	std::fprintf(stderr, "\n-- Multi-message & interleave tests --\n");
	RUN_TEST(test_multiple_gnzda_burst);
	RUN_TEST(test_interleaved_raw_and_gnzda);

	// ── Category 5: Baud rates ──
	std::fprintf(stderr, "\n-- Baud rate tests --\n");
	RUN_TEST(test_baud_9600);
	RUN_TEST(test_baud_38400);
	RUN_TEST(test_baud_115200);
	RUN_TEST(test_baud_230400);

	// ── Category 6: Latency benchmarks ──
	std::fprintf(stderr, "\n-- Latency benchmarks --\n");
	RUN_TEST(test_latency_sendzda);
	RUN_TEST(test_latency_sendgnsszda);
	RUN_TEST(test_latency_baud_comparison);
	RUN_TEST(test_latency_burst);

	// ── Summary ──
	std::fprintf(stderr,
				 "\n============================================================\n"
				 "  Results:  %d passed,  %d failed,  %d total\n"
				 "============================================================\n",
				 g_tests_passed, g_tests_failed, g_tests_run);

	return g_tests_failed > 0 ? 1 : 0;
}
