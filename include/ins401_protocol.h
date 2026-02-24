/// @file ins401_protocol.h
/// @brief INS401 Ethernet protocol constants: message IDs, payload sizes, and precomputed byte arrays.

#ifndef INS401_PROTOCOL_H
#define INS401_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>


enum class EndianType { LSB, MSB };

inline constexpr std::string_view BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";

inline constexpr std::size_t ACEINNA_PRE_AND_ID = 4;

inline constexpr std::size_t ACEINNA_HEADER_LEN = 8;

inline constexpr std::uint16_t COMMAND_START = 0x5555;

inline constexpr std::uint16_t GNSS_SOLUTION_PACKET_MESSAGE_ID = 0x0A02;

inline constexpr std::size_t GNSS_SOLUTION_PACKET_LENGTH = 77;

inline constexpr std::uint16_t REQUEST_INFO_COMMAND = 0xCC01;


[[nodiscard]] constexpr std::array<std::uint8_t, 2> ConvertUint16ToUint8(std::uint16_t value, EndianType type) {
    return (type == EndianType::LSB)
               ? std::array<std::uint8_t, 2>{
                   static_cast<std::uint8_t>(value & 0xFF),
                   static_cast<std::uint8_t>((value >> 8) & 0xFF)
               }
               : std::array<std::uint8_t, 2>{
                   static_cast<std::uint8_t>((value >> 8) & 0xFF),
                   static_cast<std::uint8_t>(value & 0xFF)
               };
}


inline constexpr auto COMMAND_START_BYTES = ConvertUint16ToUint8(COMMAND_START, EndianType::LSB);

inline constexpr auto REQUEST_INFO_COMMAND_BYTES = ConvertUint16ToUint8(REQUEST_INFO_COMMAND, EndianType::LSB);


#endif
