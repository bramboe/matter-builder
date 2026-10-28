// Host tests: the C++ protocol port must match the Python reference exactly.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "octo_protocol.h"

using octo::Bytes;

struct BuildVector {
    octo::Command command;
    Bytes data;
    Bytes expected;
};

struct ParseVector {
    Bytes message;
    bool ok;
    octo::Command command;
    Bytes data;
    bool checksum_ok;
};

struct FeatureVector {
    Bytes data;
    bool ok;
    uint32_t id;
    Bytes value;
    int value_type;  // -1 = none
};

struct PinVector {
    const char* pin;
    bool ok;
    Bytes expected;
};

struct NamedVector {
    const char* name;
    Bytes expected;
};

#include "vectors.inc"

static int failures = 0;
static int checks = 0;

static std::string hex(const Bytes& data) {
    std::string out;
    char buf[3];
    for (uint8_t b : data) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

static void check(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

static void test_build() {
    for (const auto& v : BUILD_VECTORS) {
        Bytes got = octo::build_packet(v.command, v.data);
        check(got == v.expected, "build " + hex(v.data) + ": got " + hex(got) + " want " + hex(v.expected));
    }
}

static void test_parse() {
    for (const auto& v : PARSE_VECTORS) {
        auto got = octo::parse_packet(v.message.data(), v.message.size());
        check(got.has_value() == v.ok, "parse ok " + hex(v.message));
        if (got && v.ok) {
            check(got->command == v.command, "parse command " + hex(v.message));
            check(got->data == v.data, "parse data " + hex(v.message));
        }
        check(octo::verify_response_checksum(v.message.data(), v.message.size()) == v.checksum_ok,
              "checksum " + hex(v.message));
    }
}

static void test_features() {
    for (const auto& v : FEATURE_VECTORS) {
        auto got = octo::extract_feature(v.data);
        check(got.has_value() == v.ok, "feature ok " + hex(v.data));
        if (got && v.ok) {
            check(got->id == v.id, "feature id " + hex(v.data));
            check(got->value == v.value, "feature value " + hex(v.data));
            int value_type = got->value_type ? *got->value_type : -1;
            check(value_type == v.value_type, "feature value type " + hex(v.data));
        }
    }
}

static void test_pins() {
    for (const auto& v : PIN_VECTORS) {
        auto got = octo::encode_pin(v.pin);
        check(got.has_value() == v.ok, std::string("pin ok '") + v.pin + "'");
        if (got && v.ok) {
            check(*got == v.expected, std::string("pin bytes '") + v.pin + "'");
            check(octo::is_pin_packet(got->data(), got->size()), std::string("is_pin_packet '") + v.pin + "'");
        }
    }
}

static Bytes named_helper(const std::string& name) {
    if (name == "HEAD_UP") return octo::motor_up(octo::MOTOR_HEAD);
    if (name == "HEAD_DOWN") return octo::motor_down(octo::MOTOR_HEAD);
    if (name == "FEET_UP") return octo::motor_up(octo::MOTOR_FEET);
    if (name == "FEET_DOWN") return octo::motor_down(octo::MOTOR_FEET);
    if (name == "BOTH_UP") return octo::motor_up(octo::MOTOR_BOTH);
    if (name == "BOTH_DOWN") return octo::motor_down(octo::MOTOR_BOTH);
    if (name == "STOP") return octo::motor_stop();
    if (name == "LIGHT_ON") return octo::light(true);
    if (name == "LIGHT_OFF") return octo::light(false);
    if (name == "GET_FEATURES") return octo::get_features();
    if (name == "RGBW_10_20_30_40") return octo::light_rgbw(10, 20, 30, 40);
    if (name == "RGBW_40_3C_4F_41_T6") return octo::light_rgbw(0x40, 0x3C, 0x4F, 0x41, 0x06);
    return {};
}

static void test_named() {
    for (const auto& v : NAMED_VECTORS) {
        Bytes got = named_helper(v.name);
        check(got == v.expected, std::string("named ") + v.name + ": got " + hex(got) + " want " + hex(v.expected));
    }
}

static void test_notifications() {
    using namespace octo;
    check(is_pin_required(NOTIFY_PIN_REQUIRED, sizeof(NOTIFY_PIN_REQUIRED)), "pin required");
    check(is_pin_required(NOTIFY_PIN_REQUIRED_ALT, sizeof(NOTIFY_PIN_REQUIRED_ALT)), "pin required alt");
    const uint8_t longer[] = {0x40, 0x21, 0x44, 0x00, 0x00, 0x1b, 0x40, 0x00};
    check(is_pin_required(longer, sizeof(longer)), "pin required prefix");
    check(!is_pin_required(NOTIFY_PIN_ACCEPTED, sizeof(NOTIFY_PIN_ACCEPTED)), "accepted is not required");
    check(is_pin_accepted(NOTIFY_PIN_ACCEPTED, sizeof(NOTIFY_PIN_ACCEPTED)), "pin accepted");
    check(is_pin_rejected(NOTIFY_PIN_REJECTED, sizeof(NOTIFY_PIN_REJECTED)), "pin rejected");
    check(!is_pin_accepted(NOTIFY_PIN_REJECTED, sizeof(NOTIFY_PIN_REJECTED)), "rejected is not accepted");
}

int main() {
    test_build();
    test_parse();
    test_features();
    test_pins();
    test_named();
    test_notifications();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
