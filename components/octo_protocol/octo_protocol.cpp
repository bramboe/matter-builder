#include "octo_protocol.h"

#include <cstring>

namespace octo {

namespace {

// Byte stuffing: payload byte -> value following ESCAPE_CHAR
bool escape_code(uint8_t byte, uint8_t& code) {
    switch (byte) {
        case 0x40: code = 0x01; return true;
        case 0x3C: code = 0x02; return true;
        case 0x4F: code = 0x03; return true;
        case 0x41: code = 0x04; return true;
        default: return false;
    }
}

bool unescape_code(uint8_t code, uint8_t& byte) {
    switch (code) {
        case 0x01: byte = 0x40; return true;
        case 0x02: byte = 0x3C; return true;
        case 0x03: byte = 0x4F; return true;
        case 0x04: byte = 0x41; return true;
        default: return false;
    }
}

Bytes unescape(const uint8_t* data, size_t len) {
    Bytes result;
    result.reserve(len);
    size_t i = 0;
    while (i < len) {
        uint8_t mapped;
        if (data[i] == ESCAPE_CHAR && i + 1 < len && unescape_code(data[i + 1], mapped)) {
            result.push_back(mapped);
            i += 2;
            continue;
        }
        result.push_back(data[i]);
        i += 1;
    }
    return result;
}

bool equals(const uint8_t* data, size_t len, const uint8_t* expected, size_t expected_len) {
    return len == expected_len && std::memcmp(data, expected, len) == 0;
}

}  // namespace

uint8_t calculate_checksum(const uint8_t* data, size_t len) {
    uint8_t total = 0;
    for (size_t i = 0; i < len; ++i) {
        total = static_cast<uint8_t>(total + data[i]);
    }
    return static_cast<uint8_t>((total ^ 0xFF) + 1);
}

Bytes build_packet(Command command, const Bytes& data) {
    const size_t data_len = data.size();
    // Checksum covers the unescaped packet including both framing bytes,
    // without the checksum position itself.
    Bytes summed = {PACKET_CHAR, command.c0, command.c1,
                    static_cast<uint8_t>((data_len >> 8) & 0xFF),
                    static_cast<uint8_t>(data_len & 0xFF)};
    summed.insert(summed.end(), data.begin(), data.end());
    summed.push_back(PACKET_CHAR);
    const uint8_t checksum = calculate_checksum(summed.data(), summed.size());

    Bytes payload = {command.c0, command.c1, summed[3], summed[4], checksum};
    payload.insert(payload.end(), data.begin(), data.end());

    Bytes packet;
    packet.reserve(payload.size() * 2 + 2);
    packet.push_back(PACKET_CHAR);
    for (uint8_t byte : payload) {
        uint8_t code;
        if (escape_code(byte, code)) {
            packet.push_back(ESCAPE_CHAR);
            packet.push_back(code);
        } else {
            packet.push_back(byte);
        }
    }
    packet.push_back(PACKET_CHAR);
    return packet;
}

std::optional<Packet> parse_packet(const uint8_t* message, size_t len) {
    if (len < 7) {
        return std::nullopt;
    }
    if (message[0] != PACKET_CHAR || message[len - 1] != PACKET_CHAR) {
        return std::nullopt;
    }
    Bytes payload = unescape(message + 1, len - 2);
    if (payload.size() < 5) {
        return std::nullopt;
    }
    const size_t data_len = (static_cast<size_t>(payload[2]) << 8) + payload[3];
    if (payload.size() - 5 != data_len) {
        return std::nullopt;
    }
    return Packet{{payload[0], payload[1]}, Bytes(payload.begin() + 5, payload.end())};
}

bool verify_response_checksum(const uint8_t* message, size_t len) {
    if (len < 7) {
        return false;
    }
    Bytes payload = unescape(message + 1, len - 2);
    if (payload.size() < 5) {
        return false;
    }
    Bytes check = {0x80, payload[0], payload[1], payload[2], payload[3]};
    check.insert(check.end(), payload.begin() + 5, payload.end());
    return payload[4] == calculate_checksum(check.data(), check.size());
}

std::optional<Feature> extract_feature(const Bytes& data) {
    if (data.size() < 6) {
        return std::nullopt;
    }
    Feature feature;
    feature.id = (static_cast<uint32_t>(data[0]) << 16) + (static_cast<uint32_t>(data[1]) << 8) + data[2];
    const size_t value_type_index = 5 + static_cast<size_t>(data[4]);
    if (value_type_index < data.size()) {
        feature.value_type = data[value_type_index];
    }
    const size_t value_start = value_type_index + 1;
    if (value_start > data.size()) {
        return std::nullopt;
    }
    feature.value.assign(data.begin() + value_start, data.end());
    return feature;
}

std::optional<Bytes> encode_pin(std::string_view pin) {
    if (pin.size() != 4) {
        return std::nullopt;
    }
    Bytes digits;
    for (char c : pin) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        digits.push_back(static_cast<uint8_t>(c - '0'));
    }
    return build_packet(CMD_SYSTEM_PIN, digits);
}

bool is_pin_packet(const uint8_t* data, size_t len) {
    return len >= 3 && data[1] == CMD_SYSTEM_PIN.c0 && data[2] == CMD_SYSTEM_PIN.c1;
}

bool is_pin_required(const uint8_t* data, size_t len) {
    return (len >= sizeof(NOTIFY_PIN_REQUIRED) &&
            std::memcmp(data, NOTIFY_PIN_REQUIRED, sizeof(NOTIFY_PIN_REQUIRED)) == 0) ||
           equals(data, len, NOTIFY_PIN_REQUIRED_ALT, sizeof(NOTIFY_PIN_REQUIRED_ALT));
}

bool is_pin_accepted(const uint8_t* data, size_t len) {
    return equals(data, len, NOTIFY_PIN_ACCEPTED, sizeof(NOTIFY_PIN_ACCEPTED));
}

bool is_pin_rejected(const uint8_t* data, size_t len) {
    return equals(data, len, NOTIFY_PIN_REJECTED, sizeof(NOTIFY_PIN_REJECTED));
}

Bytes motor_up(uint8_t motor_mask) { return build_packet(CMD_MOTOR_UP, {motor_mask}); }

Bytes motor_down(uint8_t motor_mask) { return build_packet(CMD_MOTOR_DOWN, {motor_mask}); }

Bytes motor_stop() { return build_packet(CMD_MOTOR_STOP); }

// Capability write to CAP_LIGHT; values from packet captures
Bytes light(bool on) {
    return build_packet(CMD_SYSTEM_SET_CAPS,
                        {0x00, 0x01, 0x02, 0x01, 0x01, 0x01, 0x01, static_cast<uint8_t>(on ? 0x01 : 0x00)});
}

Bytes light_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t value_type) {
    return build_packet(CMD_SYSTEM_SET_CAPS,
                        {0x00, 0x01, 0x04, 0x00, 0x01, 0x01, value_type, r, g, b, w, 0xFF});
}

Bytes get_features() { return build_packet(CMD_SYSTEM_GET_CAPS); }

}  // namespace octo
