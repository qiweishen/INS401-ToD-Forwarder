#include "ethernet_socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <memory>
#include <net/if.h>
#include <string>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "ins401_protocol.h"


EthernetSocket::EthernetSocket(std::string interface_name, const MacAddress &target_mac,
                               const std::size_t recv_buffer_size,
                               const bool enable_bpf) : interface_name_(std::move(interface_name)),
                                                        target_mac_(target_mac),
                                                        recv_buffer_size_(recv_buffer_size), enable_bpf_(enable_bpf) {
    CreateSocket();
    if (enable_bpf_) {
        Ethernet::SetupBpfFilter(target_mac_, local_mac_, socket_fd_);
    }
    SetupEpoll();
}

EthernetSocket::~EthernetSocket() {
    CloseEthernetSocket();
}


std::ptrdiff_t EthernetSocket::Send(const std::vector<uint8_t> &frame) const {
    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index_;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_halen = ETH_ALEN;
    std::memcpy(sll.sll_addr, target_mac_.data(), ETH_ALEN);

    return sendto(socket_fd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr *>(&sll), sizeof(sll));
}


std::optional<EthernetFrame> EthernetSocket::Receive(const int timeout_ms) const {
    epoll_event ev{};
    int ret = epoll_wait(epoll_fd_, &ev, 1, timeout_ms);
    if (ret <= 0) {
        return std::nullopt;
    }

    std::array<std::uint8_t, kMaxFrameSize> buffer{};
    const ssize_t len = recv(socket_fd_, buffer.data(), buffer.size(), 0);
    if (len <= 0) {
        return std::nullopt;
    }

    EthernetFrame frame;
    frame.assign(buffer.begin(), buffer.begin() + len);
    return frame;
}


std::vector<EthernetFrame> EthernetSocket::ReceiveBatch(std::size_t max_frames) const {
    std::vector<EthernetFrame> frames;
    frames.reserve(max_frames);

    std::array<std::uint8_t, kMaxFrameSize> buffer{};

    while (frames.size() < max_frames) {
        const ssize_t len = recv(socket_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (len <= 0) {
            break;
        }

        EthernetFrame frame;
        frame.assign(buffer.begin(), buffer.begin() + len);
        frames.push_back(std::move(frame));
    }

    return frames;
}


void EthernetSocket::CreateSocket() {
    socket_fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socket_fd_ < 0) return;

    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, interface_name_.c_str(), IFNAMSIZ - 1);

    if (ioctl(socket_fd_, SIOCGIFINDEX, &ifr) < 0) return;
    if_index_ = ifr.ifr_ifindex;

    if (ioctl(socket_fd_, SIOCGIFHWADDR, &ifr) < 0) return;
    std::memcpy(local_mac_.data(), ifr.ifr_hwaddr.sa_data, kMacAddressSize);

    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index_;
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) return;

    if (recv_buffer_size_ > 0) {
        int buf_size = static_cast<int>(std::min(recv_buffer_size_,
                                                 static_cast<size_t>(std::numeric_limits<int>::max())));
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    }
}


void EthernetSocket::SetupEpoll() {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) return;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = socket_fd_;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket_fd_, &ev) < 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}


void EthernetSocket::CloseEthernetSocket() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}


namespace Ethernet {
    std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces() {
        std::vector<std::pair<std::string, std::string> > interfaces;

        ifaddrs *ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == -1) {
            return interfaces;
        }

        auto ifaddr_guard = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>(ifaddr, freeifaddrs);
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return interfaces;
        }
        FdGuard fd_guard(fd);

        for (ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_name) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0 || strcmp(ifa->ifa_name, "lo0") == 0) continue;

            if (ifa->ifa_addr->sa_family == AF_PACKET) {
                const auto *sll = reinterpret_cast<const sockaddr_ll *>(ifa->ifa_addr);
                if (sll->sll_halen != 6) continue;

                ifreq ifr{};
                strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
                ifr.ifr_name[IFNAMSIZ - 1] = '\0';

                if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
                    if ((ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING)) {
                        interfaces.emplace_back(ifa->ifa_name, ParseMacAddress(sll->sll_addr));
                    }
                }
            }
        }
        return interfaces;
    }


    bool SetupBpfFilter(const MacAddress target_mac, const MacAddress local_mac, const int socket_fd) {
        std::uint32_t target_mac_hi, local_mac_hi;
        std::uint16_t target_mac_lo, local_mac_lo;

        std::memcpy(&target_mac_hi, target_mac.data(), 4);
        std::memcpy(&target_mac_lo, target_mac.data() + 4, 2);
        std::memcpy(&local_mac_hi, local_mac.data(), 4);
        std::memcpy(&local_mac_lo, local_mac.data() + 4, 2);

        target_mac_hi = ntohl(target_mac_hi);
        target_mac_lo = ntohs(target_mac_lo);
        local_mac_hi = ntohl(local_mac_hi);
        local_mac_lo = ntohs(local_mac_lo);

        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_hi, 0, 6),
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_lo, 0, 4),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_hi, 0, 2),
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_lo, 8, 0),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_hi, 0, 5),
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_lo, 0, 3),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_hi, 0, 1),
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_lo, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, 0xFFFF),
            BPF_STMT(BPF_RET | BPF_K, 0),
        };

        sock_fprog prog = {
            .len = static_cast<unsigned short>(std::size(filter)),
            .filter = filter,
        };

        return setsockopt(socket_fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) >= 0;
    }


    std::vector<uint8_t> BuildAceinnaPacket(const MacAddress target_mac, const MacAddress local_mac,
                                            const std::array<uint8_t, 2> message_id, const uint8_t *payload,
                                            const size_t payload_length) {
        std::vector<uint8_t> frame;
        frame.insert(frame.end(), target_mac.data(), target_mac.data() + kMacAddressSize);
        frame.insert(frame.end(), local_mac.data(), local_mac.data() + kMacAddressSize);

        std::vector<uint8_t> aceinna_packet;
        aceinna_packet.insert(aceinna_packet.end(), COMMAND_START_BYTES.data(), COMMAND_START_BYTES.data() + 2);
        aceinna_packet.insert(aceinna_packet.end(), message_id.data(), message_id.data() + 2);

        const auto length = static_cast<uint32_t>(payload != nullptr ? payload_length : 0);
        aceinna_packet.push_back(static_cast<uint8_t>(length & 0xFF));
        aceinna_packet.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        aceinna_packet.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
        aceinna_packet.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));

        if (payload != nullptr && payload_length > 0) {
            aceinna_packet.insert(aceinna_packet.end(), payload, payload + payload_length);
        }

        const uint16_t crc16 = Ethernet::CRC::CalculateINS401_CRC16(&aceinna_packet[2], aceinna_packet.size() - 2);
        aceinna_packet.push_back(static_cast<uint8_t>(crc16 & 0xFF));
        aceinna_packet.push_back(static_cast<uint8_t>((crc16 >> 8) & 0xFF));

        auto eth_payload_length = static_cast<uint16_t>(aceinna_packet.size());
        frame.push_back(static_cast<uint8_t>(eth_payload_length & 0xFF));
        frame.push_back(static_cast<uint8_t>((eth_payload_length >> 8) & 0xFF));

        frame.insert(frame.end(), aceinna_packet.begin(), aceinna_packet.end());

        if (aceinna_packet.size() < kMinPayloadSize) {
            frame.insert(frame.end(), kMinPayloadSize - aceinna_packet.size(), 0x00);
        }
        return frame;
    }


    std::string ParseMacAddress(const std::array<uint8_t, 6> &mac) {
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return buf;
    }

    std::string ParseMacAddress(const uint8_t *mac_ptr) {
        if (!mac_ptr) return "00:00:00:00:00:00";
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                      mac_ptr[0], mac_ptr[1], mac_ptr[2], mac_ptr[3], mac_ptr[4], mac_ptr[5]);
        return buf;
    }


    std::array<uint8_t, 6> FormatMACAddress(std::string mac_str) {
        std::string hex_only;
        hex_only.reserve(12);
        for (char c : mac_str) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                hex_only += c;
            }
        }
        std::array<uint8_t, 6> result{};
        if (hex_only.length() == 12) {
            for (size_t i = 0; i < 6; ++i) {
                result[i] = static_cast<uint8_t>(std::stoul(hex_only.substr(i * 2, 2), nullptr, 16));
            }
        }
        return result;
    }


    namespace CRC {
        uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length) {
            uint16_t crc = 0x1D0F;
            for (int i = 0; i < length; i++) {
                crc ^= buf[i] << 8;
                for (int j = 0; j < 8; j++) {
                    if (crc & 0x8000) {
                        crc = (crc << 1) ^ 0x1021;
                    } else {
                        crc = crc << 1;
                    }
                }
            }
            return ((crc << 8) & 0xFF00) | ((crc >> 8) & 0xFF);
        }


        uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes) {
            constexpr uint32_t kPoly = 0x1864CFB;
            uint32_t crc = 0;
            const auto *bytes = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < nBytes; ++i) {
                crc ^= static_cast<uint32_t>(bytes[i]) << 16;
                for (int j = 0; j < 8; ++j) {
                    crc <<= 1;
                    if (crc & 0x1000000) crc ^= kPoly;
                }
            }
            return crc & 0xFFFFFF;
        }
    } // namespace CRC
} // namespace Ethernet
