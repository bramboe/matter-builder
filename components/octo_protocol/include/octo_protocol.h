// Octo bed BLE packet protocol (standard variant).
//
// C++ port of custom_components/octo_bed/protocol.py from
// https://github.com/bramboe/octo-bed. Both implementations are kept equal by
// the golden vectors in test/host (generated from the Python code).
//
// Packet format: [0x40, escaped(cmd0, cmd1, len_hi, len_lo, checksum, ...data), 0x40]
//
// The checksum is ((sum of all unescaped bytes except the checksum itself) XOR 0xFF) + 1.
// Response packets from the bed use 0x80 instead of the leading 0x40 in the
// checksum calculation. Payload bytes 0x40/0x3C/0x4F/0x41 are escaped as
// 0x3C followed by a mapped value (byte stuffing).
//
// Protocol reverse-engineered by the Home Assistant community:
// https://community.home-assistant.io/t/540790

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace octo {

using Bytes = std::vector<uint8_t>;

struct Command {
    uint8_t c0;
    uint8_t c1;
    bool operator==(const Command& other) const { return c0 == other.c0 && c1 == other.c1; }
};

constexpr uint8_t PACKET_CHAR = 0x40;
constexpr uint8_t ESCAPE_CHAR = 0x3C;

// Command identifiers (cmd0, cmd1)
constexpr Command CMD_MOTOR_UP{0x02, 0x70};
constexpr Command CMD_MOTOR_DOWN{0x02, 0x71};
constexpr Command CMD_MOTOR_MEMPOS{0x02, 0x72};          // recall memory preset, data=[slot]
constexpr Command CMD_MOTOR_STOP{0x02, 0x73};
constexpr Command CMD_CONFIG_SAVE_MOTORPOS{0x10, 0x70};  // save memory preset, data=[slot]
constexpr Command CMD_CONFIG_SET_DRIVEMODE{0x10, 0x71};  // data=[0]=single, [1]=sync
constexpr Command CMD_CONFIG_GET_DRIVEMODE{0x10, 0x72};
constexpr Command CMD_SYSTEM_PIN{0x20, 0x43};            // data=[d1, d2, d3, d4]
constexpr Command CMD_SYSTEM_GET_CAPS{0x20, 0x71};       // feature discovery
constexpr Command CMD_SYSTEM_SET_CAPS{0x20, 0x72};       // write capability value (light etc.)

// Responses from the bed
constexpr Command RSP_FEATURE{0x21, 0x71};

// Motor bit masks
constexpr uint8_t MOTOR_HEAD = 0x02;
constexpr uint8_t MOTOR_FEET = 0x04;
constexpr uint8_t MOTOR_BOTH = MOTOR_HEAD | MOTOR_FEET;

// Feature/capability IDs (3-byte big-endian in responses)
constexpr uint32_t FEATURE_MOTORCOUNT = 0x000001;
constexpr uint32_t FEATURE_MEMCOUNT = 0x000002;
constexpr uint32_t FEATURE_PIN = 0x000003;
constexpr uint32_t FEATURE_SYNCHRO = 0x000101;
constexpr uint32_t FEATURE_LIGHT = 0x000102;
constexpr uint32_t FEATURE_LIGHT_RGBWI = 0x000104;
constexpr uint32_t FEATURE_END = 0xFFFFFF;  // end-of-feature-list sentinel

constexpr uint8_t DRIVEMODE_SINGLE = 0x00;
constexpr uint8_t DRIVEMODE_SYNC = 0x01;

// Fixed notifications (from packet captures)
// 40214400001b40 = periodic keep-alive request from bed
// 40217f0000e040 = initial "no PIN given" / auth required
constexpr uint8_t NOTIFY_PIN_REQUIRED[] = {0x40, 0x21, 0x44, 0x00, 0x00, 0x1b, 0x40};
constexpr uint8_t NOTIFY_PIN_REQUIRED_ALT[] = {0x40, 0x21, 0x7f, 0x00, 0x00, 0xe0, 0x40};
constexpr uint8_t NOTIFY_PIN_ACCEPTED[] = {0x40, 0x21, 0x43, 0x00, 0x01, 0x1a, 0x01, 0x40};
constexpr uint8_t NOTIFY_PIN_REJECTED[] = {0x40, 0x21, 0x43, 0x00, 0x01, 0x1b, 0x00, 0x40};

struct Packet {
    Command command;
    Bytes data;
};

struct Feature {
    uint32_t id;
    Bytes value;
    std::optional<uint8_t> value_type;
};

// Return the Octo checksum over the given (unescaped) bytes.
uint8_t calculate_checksum(const uint8_t* data, size_t len);

// Build a command packet with checksum and byte stuffing.
Bytes build_packet(Command command, const Bytes& data = {});

// Parse a response packet. A checksum mismatch does not reject the packet:
// some control boxes send packets with a zeroed checksum.
std::optional<Packet> parse_packet(const uint8_t* message, size_t len);

// Return true if the response checksum matches (0x80-based).
bool verify_response_checksum(const uint8_t* message, size_t len);

// Extract one capability entry from a 0x21 0x71 response.
// Data layout: 3-byte feature ID, 1 flag byte, 1 skip-length byte,
// skipped bytes, 1 value-type byte, value bytes.
std::optional<Feature> extract_feature(const Bytes& data);

// Encode a 4-digit PIN into an authentication packet; nullopt if invalid.
std::optional<Bytes> encode_pin(std::string_view pin);

// Return true if this outgoing packet contains the PIN (for log masking).
bool is_pin_packet(const uint8_t* data, size_t len);

// Notification classification as done by the Home Assistant integration.
bool is_pin_required(const uint8_t* data, size_t len);
bool is_pin_accepted(const uint8_t* data, size_t len);
bool is_pin_rejected(const uint8_t* data, size_t len);

// Ready-made commands
Bytes motor_up(uint8_t motor_mask);
Bytes motor_down(uint8_t motor_mask);
Bytes motor_stop();
Bytes light(bool on);
Bytes light_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t value_type = 0x05);
Bytes get_features();

}  // namespace octo
