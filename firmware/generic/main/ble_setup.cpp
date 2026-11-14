#include "ble_setup.h"

#include <cctype>
#include <cstring>
#include <memory>

#include "cJSON.h"

namespace ble_setup {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t MAX_BYTES = 64;

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex(const std::string& text, Bytes& bytes, size_t max) {
    if (text.empty() || text.size() % 2 || text.size() > max * 2) return false;
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]), low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes.push_back(static_cast<uint8_t>(high * 16 + low));
    }
    return true;
}

const cJSON* field(const cJSON* node, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(node, name);
}

bool string_field(const cJSON* node, const char* name, std::string& out) {
    const auto* value = field(node, name);
    if (!value) return true;
    if (!cJSON_IsString(value) || !*value->valuestring) return false;
    out = value->valuestring;
    return true;
}

bool hex_field(const cJSON* node, const char* name, Bytes& out) {
    std::string value;
    return string_field(node, name, value) && (value.empty() || hex(value, out, 32));
}

bool uuid(const std::string& value) {
    if (value.size() != 4 && value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const bool dash = value.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? value[i] != '-' : nibble(value[i]) < 0) return false;
    }
    return true;
}

bool secret_name(const std::string& name) {
    if (name.empty() || name.size() > 12 || name.front() < 'a' || name.front() > 'z') return false;
    for (const char c : name) if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') return false;
    return true;
}

bool parse_step(const cJSON* node, Step& step) {
    if (!cJSON_IsObject(node) || !string_field(node, "type", step.name)) return false;
    constexpr const char* names[] = {"pair", "discover", "delay", "subscribe", "write", "read", "wait"};
    size_t kind = 0;
    while (kind < std::size(names) && step.name != names[kind]) ++kind;
    if (kind == std::size(names)) return false;
    step.type = static_cast<Type>(kind);
    const auto* ms = field(node, "ms");
    if (!cJSON_IsNumber(ms) || ms->valuedouble != ms->valueint || ms->valueint < 1 || ms->valueint > 10000) return false;
    step.ms = ms->valueint;
    if (!string_field(node, "service", step.service) || !string_field(node, "characteristic", step.characteristic)) return false;
    if ((!step.service.empty() || !step.characteristic.empty()) && (!uuid(step.service) || !uuid(step.characteristic))) return false;
    if (!hex_field(node, "bytes", step.bytes) || !hex_field(node, "expect", step.expect) ||
        !hex_field(node, "reject", step.reject) || !hex_field(node, "escapes", step.escapes)) return false;
    Bytes frame, escape;
    if (!hex_field(node, "frame", frame) || frame.size() > 1 || !hex_field(node, "escape_byte", escape) || escape.size() > 1 ||
        step.escapes.size() % 2 || (escape.empty() != step.escapes.empty())) return false;
    if (!frame.empty()) step.frame = frame[0];
    if (!escape.empty()) step.escape_byte = escape[0];
    std::string encoding, checksum;
    if (!string_field(node, "secret", step.secret) || !string_field(node, "encoding", encoding) ||
        !string_field(node, "checksum", checksum)) return false;
    if (!step.secret.empty() && !secret_name(step.secret)) return false;
    if (!encoding.empty() && encoding != "hex" && encoding != "digits") return false;
    if (!checksum.empty() && checksum != "negative-sum8") return false;
    step.digits = encoding == "digits";
    step.checksum = !checksum.empty();
    const auto* indicate = field(node, "indicate");
    if (indicate && !cJSON_IsBool(indicate)) return false;
    step.indicate = cJSON_IsTrue(indicate);
    if (step.type == Type::write && step.bytes.empty() && step.secret.empty()) return false;
    return step.type != Type::wait || (!step.expect.empty() && step.expect != step.reject);
}
}  // namespace

bool parse(const std::string& json, std::vector<Step>& steps) {
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithOpts(json.c_str(), nullptr, true), cJSON_Delete);
    if (!cJSON_IsArray(root.get()) || cJSON_GetArraySize(root.get()) > 10) return false;
    std::vector<Step> parsed;
    const cJSON* node;
    int budget = 0;
    bool discovered = false;
    cJSON_ArrayForEach(node, root.get()) {
        Step step{};
        if (!parse_step(node, step)) return false;
        budget += step.ms;
        if (budget > 40000) return false;
        if (step.type == Type::discover) discovered = true;
        if (step.type >= Type::subscribe && step.service.empty() && !discovered) return false;
        if (step.type == Type::wait) {
            bool subscribed = false;
            for (const Step& previous : parsed) {
                if (previous.type == Type::subscribe && previous.service == step.service &&
                    previous.characteristic == step.characteristic) subscribed = true;
            }
            if (!subscribed) return false;
        }
        parsed.push_back(std::move(step));
    }
    if (!parsed.empty() && !discovered) return false;
    steps = std::move(parsed);
    return true;
}

bool encode(const Step& step, const std::string& secret, Bytes& packet) {
    Bytes data;
    if (!step.secret.empty()) {
        if (secret.empty()) return false;
        if (step.digits) {
            for (char c : secret) {
                if (c < '0' || c > '9') return false;
                data.push_back(static_cast<uint8_t>(c - '0'));
            }
        } else if (!hex(secret, data, 32)) {
            return false;
        }
    }
    Bytes body = step.bytes;
    if (step.checksum) {
        unsigned sum = step.frame < 0 ? 0 : step.frame * 2;
        for (uint8_t byte : body) sum += byte;
        for (uint8_t byte : data) sum += byte;
        body.push_back(static_cast<uint8_t>(0u - sum));
    }
    body.insert(body.end(), data.begin(), data.end());
    if (step.frame >= 0) packet.push_back(static_cast<uint8_t>(step.frame));
    for (uint8_t byte : body) {
        size_t i = 0;
        while (i < step.escapes.size() && step.escapes[i] != byte) i += 2;
        if (i == step.escapes.size()) {
            packet.push_back(byte);
        } else {
            packet.push_back(static_cast<uint8_t>(step.escape_byte));
            packet.push_back(step.escapes[i + 1]);
        }
    }
    if (step.frame >= 0) packet.push_back(static_cast<uint8_t>(step.frame));
    return !packet.empty() && packet.size() <= MAX_BYTES;
}

}  // namespace ble_setup
