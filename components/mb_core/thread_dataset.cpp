#include "thread_dataset.h"

#include <cstdio>
#include <cstring>

#include <lib/support/ThreadOperationalDataset.h>

#include "nvs.h"
#include "mb_core.h"


namespace thread_dataset {

namespace {

const char* NVS_NAMESPACE = "thread";
const char* NVS_KEY = "dataset";

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns the number of bytes, or 0 when the text is not valid hex.
size_t parse_hex(const char* text, uint8_t* out, size_t max) {
    if (text == nullptr) {
        return 0;
    }
    const size_t length = std::strlen(text);
    if (length == 0 || length % 2 != 0 || length / 2 > max) {
        return 0;
    }
    for (size_t i = 0; i < length / 2; ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        out[i] = static_cast<uint8_t>(high << 4 | low);
    }
    return length / 2;
}

size_t stored(uint8_t* tlvs, size_t max) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    size_t length = max;
    if (nvs_get_blob(handle, NVS_KEY, tlvs, &length) != ESP_OK) {
        length = 0;
    }
    nvs_close(handle);
    return length;
}

bool store(const uint8_t* tlvs, size_t length) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = length == 0 ? nvs_erase_key(handle, NVS_KEY) : nvs_set_blob(handle, NVS_KEY, tlvs, length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

// A dataset a bridge can join: it names the network and holds its key
bool usable(const uint8_t* tlvs, size_t length, chip::Thread::OperationalDataset& dataset) {
    return length > 0 && dataset.Init(chip::ByteSpan(tlvs, length)) == CHIP_NO_ERROR && dataset.IsCommissioned();
}

std::string json_text(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out + "\"";
}

}  // namespace

size_t wanted(uint8_t* tlvs, size_t max, const char** source) {
    chip::Thread::OperationalDataset dataset;
    size_t length = stored(tlvs, max);
    if (usable(tlvs, length, dataset)) {
        *source = "page";
        return length;
    }
    length = parse_hex(mb_core::app().thread_dataset_fallback, tlvs, max);
    if (usable(tlvs, length, dataset)) {
        *source = "secrets";
        return length;
    }
    *source = "none";
    return 0;
}

std::string command(const std::vector<std::string>& args) {
    uint8_t tlvs[chip::Thread::kSizeOperationalDataset];
    if (args.size() == 3 && args[1] == "dataset") {
        chip::Thread::OperationalDataset dataset;
        const size_t length = parse_hex(args[2].c_str(), tlvs, sizeof(tlvs));
        if (!usable(tlvs, length, dataset)) {
            return "not a Thread dataset: paste the hex text from Home Assistant (Thread > (i))\n";
        }
        return store(tlvs, length) ? "ok, joined at the next start\n" : "storing failed\n";
    }
    if (args.size() == 2 && args[1] == "clear") {
        return store(nullptr, 0) ? "ok\n" : "storing failed\n";
    }
    const char* source = "none";
    const size_t length = wanted(tlvs, sizeof(tlvs), &source);
    char name[chip::Thread::kSizeNetworkName + 1] = {};
    uint16_t channel = 0;
    uint16_t pan_id = 0;
    if (length > 0) {
        chip::Thread::OperationalDataset dataset;
        dataset.Init(chip::ByteSpan(tlvs, length));
        dataset.GetNetworkName(name);
        dataset.GetChannel(channel);
        dataset.GetPanId(pan_id);
    }
    if (args.size() == 2 && args[1] == "json") {
        char pan[8];
        std::snprintf(pan, sizeof(pan), "0x%04X", pan_id);
        return std::string("{\"source\":\"") + source + "\",\"name\":" + json_text(name) +
               ",\"channel\":" + std::to_string(channel) + ",\"pan_id\":\"" + pan + "\"}";
    }
    if (length == 0) {
        return "thread: no network set (thread dataset <hex>)\n";
    }
    return std::string("thread: '") + name + "', channel " + std::to_string(channel) + " (" + source + ")\n";
}

}  // namespace thread_dataset
