#include "setup_code.h"

#include <cstdio>
#include <cstring>

#include <crypto/CHIPCryptoPAL.h>
#include <esp_matter_providers.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include "esp_log.h"
#include "esp_random.h"
#include "qrcode.h"
#include "nvs.h"

namespace setup_code {

namespace {

const char* TAG = "setup";
const char* NVS_NAMESPACE = "octo_setup";
constexpr uint32_t ITERATIONS = 1000;
constexpr size_t SALT_LENGTH = 32;

class Provider : public chip::DeviceLayer::CommissionableDataProvider {
public:
    void load_or_create() {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            ESP_LOGE(TAG, "NVS unavailable, pairing code not stored");
            create();
            return;
        }
        size_t salt_length = sizeof(salt);
        if (nvs_get_u32(handle, "pin", &passcode) == ESP_OK && nvs_get_u16(handle, "disc", &discriminator) == ESP_OK &&
            nvs_get_blob(handle, "salt", salt, &salt_length) == ESP_OK && salt_length == SALT_LENGTH &&
            chip::SetupPayload::IsValidSetupPIN(passcode)) {
            verifier_length = sizeof(verifier);
            if (nvs_get_blob(handle, "ver", verifier, &verifier_length) != ESP_OK) {
                verifier_length = 0;  // computed when first needed
            }
        } else {
            create();
            nvs_set_u32(handle, "pin", passcode);
            nvs_set_u16(handle, "disc", discriminator);
            nvs_set_blob(handle, "salt", salt, SALT_LENGTH);
            nvs_commit(handle);
            ESP_LOGI(TAG, "New pairing code generated");
        }
        nvs_close(handle);
    }

    CHIP_ERROR GetSetupDiscriminator(uint16_t& value) override {
        value = discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t& value) override {
        value = ITERATIONS;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan& buffer) override {
        return chip::CopySpanToMutableSpan(chip::ByteSpan(salt, SALT_LENGTH), buffer);
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan& buffer, size_t& length) override {
        if (verifier_length == 0) {
            // Derived from the passcode once (PBKDF2), then kept in NVS
            chip::Crypto::Spake2pVerifier spake;
            ReturnErrorOnFailure(spake.Generate(ITERATIONS, chip::ByteSpan(salt, SALT_LENGTH), passcode));
            chip::MutableByteSpan out(verifier);
            ReturnErrorOnFailure(spake.Serialize(out));
            verifier_length = out.size();
            nvs_handle_t handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
                nvs_set_blob(handle, "ver", verifier, verifier_length);
                nvs_commit(handle);
                nvs_close(handle);
            }
        }
        length = verifier_length;
        return chip::CopySpanToMutableSpan(chip::ByteSpan(verifier, verifier_length), buffer);
    }

    CHIP_ERROR GetSetupPasscode(uint32_t& value) override {
        value = passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    bool loaded() const { return passcode != 0; }

private:
    uint32_t passcode = 0;
    uint16_t discriminator = 0;
    uint8_t salt[SALT_LENGTH] = {};
    uint8_t verifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
    size_t verifier_length = 0;

    void create() {
        do {
            passcode = 1 + esp_random() % chip::kSetupPINCodeMaximumValue;
        } while (!chip::SetupPayload::IsValidSetupPIN(passcode));
        discriminator = esp_random() & 0x0FFF;
        esp_fill_random(salt, SALT_LENGTH);
        verifier_length = 0;
    }
};

Provider provider;

// esp_qrcode reports the result through a callback without context
std::string* qr_path = nullptr;
int qr_size = 0;

void qr_to_path(esp_qrcode_handle_t qrcode) {
    qr_size = esp_qrcode_get_size(qrcode);
    for (int y = 0; y < qr_size; ++y) {
        for (int x = 0; x < qr_size;) {
            if (!esp_qrcode_get_module(qrcode, x, y)) {
                ++x;
                continue;
            }
            int run = 1;
            while (x + run < qr_size && esp_qrcode_get_module(qrcode, x + run, y)) {
                ++run;
            }
            char part[64];
            std::snprintf(part, sizeof(part), "M%d %dh%dv1h-%dz", x, y, run, run);
            *qr_path += part;
            x += run;
        }
    }
}

}  // namespace

bool codes(std::string& qr, std::string& manual) {
    if (!provider.loaded()) {
        provider.load_or_create();
    }
    chip::SetupPayload payload;
    payload.vendorID = CONFIG_DEVICE_VENDOR_ID;
    payload.productID = CONFIG_DEVICE_PRODUCT_ID;
    payload.rendezvousInformation.SetValue(
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));
    uint16_t discriminator = 0;
    uint32_t passcode = 0;
    if (provider.GetSetupDiscriminator(discriminator) != CHIP_NO_ERROR ||
        provider.GetSetupPasscode(passcode) != CHIP_NO_ERROR) {
        return false;
    }
    payload.discriminator.SetLongValue(discriminator);
    payload.setUpPINCode = passcode;
    return chip::QRCodeSetupPayloadGenerator(payload).payloadBase38Representation(qr) == CHIP_NO_ERROR &&
           chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(manual) == CHIP_NO_ERROR;
}

std::string qr_svg_path(const char* qr, int& size) {
    std::string path;
    esp_qrcode_config_t config = {};
    config.display_func = qr_to_path;
    config.max_qrcode_version = 6;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    qr_path = &path;
    if (esp_qrcode_generate(&config, qr) != ESP_OK) {
        path.clear();
    }
    qr_path = nullptr;
    size = path.empty() ? 0 : qr_size;
    return path;
}

void install() {
    provider.load_or_create();
    esp_matter::set_custom_commissionable_data_provider(&provider);
}

}  // namespace setup_code
